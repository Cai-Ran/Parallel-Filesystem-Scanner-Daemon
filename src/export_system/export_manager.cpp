#include <thread>
#include "export_manager.h"
#include "async_logger.h"
#include "metrics.h"
#include "config.h"
#include "index_writer.h"
#include "index_reader.h"
#include "scan_table.h"
#include "scan_diff.h"


namespace {
    JobQueue<FileEvent>::QueueMetrics result_queue_metrics() {
        JobQueue<FileEvent>::QueueMetrics metrics{};
        metrics.queued_number = &Metrics::measurement().export_result_pending;
        return metrics;
    }
    JobQueue<DeleteTask>::QueueMetrics delete_queue_metrics() {
        JobQueue<DeleteTask>::QueueMetrics metrics{};
        metrics.queued_number = &Metrics::measurement().export_delete_pending;
        return metrics;
    }
};

ExportManager::ExportManager() 
    : db_path(Config::cfg().db().db_path),
      WRITE_BATCH_SIZE(Config::cfg().db().batch_size),
      result_queue(
        JobQueue<FileEvent>::Fifo, result_queue_metrics(), 
        Config::cfg().exportmanager().result_que_size),
      delete_queue(
        JobQueue<DeleteTask>::Fifo, delete_queue_metrics(), 
        Config::cfg().exportmanager().delete_que_size)
    {
        Metrics::measurement().const_result_que_size = Config::cfg().exportmanager().result_que_size;
        Metrics::measurement().const_delete_que_size = Config::cfg().exportmanager().delete_que_size;
    }


bool
ExportManager::start() {

    if (result_thread.joinable() || delete_thread.joinable())
        return true;

    // Pre-create all tables with a single connection before launching threads.
    // Without this, both threads race to set WAL mode to create a fresh database file
    // and one hits SQLITE_BUSY -> uncaught std::runtime_error -> std::terminate.
    try {
        DatabaseConnection db_con(db_path);
        ScanTable    init_scan(db_con);

        // load completed scans in db into map for frontend checking
        int limit = 1000;
        int offset = 0;

        while (true) {
            int loaded = 0;
        
            init_scan.get_all(limit, offset,
                [&](const ScanTaskRow& row) {
                    {
                        std::lock_guard<std::mutex> lock(map_mtx);
                        exported_map.emplace(row.scan_id);
                    }
                    loaded++;
                }
            );

            if (loaded < limit) break;  //last page
            offset += limit;
        }


    } catch (const std::exception& e) {
        AsyncLogger::logger().error("ExportManager::start - DB schema init failed: " + std::string(e.what()));
        return false;
    }

    try {
        result_thread = std::thread(&ExportManager::write_result_to_db, this);
        delete_thread = std::thread(&ExportManager::mark_deleted_in_db, this);

    } catch (const std::exception& e) {
        AsyncLogger::logger().error("ExportManager::start() - create thread failed");
        shutdown();
        return false;
    }

    return true;
}


void 
ExportManager::shutdown() {
    stop_flag = true;

    //close delete queue first to prevent result thread blocking
    result_queue.shutdown();
    if (result_thread.joinable())
        result_thread.join();

    delete_queue.shutdown();
    if (delete_thread.joinable())
        delete_thread.join();

}


void 
ExportManager::insert_scan_task(ScanTaskRow&& data) {
    DatabaseConnection db_con(db_path);
    ScanTable scan_table(db_con);
    scan_table.upsert(data);
}

// we actually insert to db at end of scan; update_scan_finish unused
void 
ExportManager::update_scan_finish(uint64_t scan_id, time_t finish_time) {
    DatabaseConnection db_con(db_path);
    ScanTable scan_table(db_con);
    scan_table.update_end(scan_id, finish_time);
}

void 
ExportManager::push_result_queue(FileEvent&& event) {
    result_queue.push(std::move(event));
}

void 
ExportManager::write_result_to_db() {
    try {

        DatabaseConnection db_con(db_path);
        IndexWriter writer(db_con);

        int batch_count = 0;

        std::function<void()> commit_batch = [&]() {
            if (batch_count > 0) {
                writer.end_transaction();
                Metrics::measurement().export_result_running.fetch_sub(batch_count);
                Metrics::measurement().export_result_finished.fetch_add(batch_count);
                batch_count = 0;
            }
        };


        FileEvent result;

        while (result_queue.pop(result)) {
        
            if (result.end_signal.is_sentinel) {
                commit_batch();
                
                DeleteTask task(result.scan_id, result.path, result.end_signal.is_canceled);
                delete_queue.push(std::move(task));
                continue;
            }

            if (batch_count == 0) {
                writer.begin_transaction();
            }

            Metrics::measurement().export_result_running.fetch_add(1);
            writer.upsert(result);
            batch_count++;

            if (batch_count >= WRITE_BATCH_SIZE) {
                commit_batch();
            }

            // Metrics::measurement().export_result_running.fetch_sub(1);
            // Metrics::measurement().export_result_finished.fetch_add(1);
        }

    } catch (const std::exception& e) {
        AsyncLogger::logger().error("write_result_to_db - fatal: " + std::string(e.what()));
        delete_queue.shutdown();    //not to block delete_thread
    }
}


void 
ExportManager::mark_deleted_in_db() {
    try {
        DatabaseConnection db_con(db_path);
        IndexWriter writer(db_con);
        IndexReader reader(db_con);
        ScanDiff    differ(db_con);
        ScanTable scan_writer(db_con);

        DeleteTask delete_task;
        while (delete_queue.pop(delete_task)) {

            Metrics::measurement().export_delete_running.fetch_add(1);

            writer.mark_deleted(delete_task);
            reader.find_scan_diff_and_upsert_scandiff(delete_task.scan_id, differ);
            scan_writer.upsert_count(delete_task.scan_id);

            {   
                std::lock_guard<std::mutex> lock(map_mtx);
                exported_map.emplace(delete_task.scan_id);
            }
            
            notify_export_done(delete_task.submit_root);

            Metrics::measurement().export_delete_running.fetch_sub(1);
            Metrics::measurement().export_delete_finished.fetch_add(1);
        }
    } catch (const std::exception& e) {
        AsyncLogger::logger().error("mark_deleted_in_db - fatal: " + std::string(e.what()));
    }
}

std::vector<bool>
ExportManager::check_exported(const std::vector<uint64_t>& scan_ids) {
    std::lock_guard<std::mutex> lock(map_mtx);

    std::vector<bool> results;
    results.reserve(scan_ids.size());
    
    for (size_t i=0; i<scan_ids.size(); ++i) {
        std::unordered_set<uint64_t>::iterator it = exported_map.find(scan_ids[i]);
        if (it == exported_map.end()) {
            // AsyncLogger::logger().debug("ExportManager::check_exported - scan_id " + std::to_string(scan_ids[i]) + " not in export_map; maybe exporting, or pending -> canceled");
            results.push_back(0);
        }
        else    results.push_back(1);
    }

    return results;
}

void 
ExportManager::set_scheduler_callback(std::function<void(const std::string&)> fn) {
    notify_export_done = std::move(fn);
}