#include <export_manager.h>
#include <exporter.h>
#include <index_reporter.h>
#include <async_logger.h>
#include <metrics.h>

// ============
// MANAGER API
// ============

void            //blocking wait pop
ExportManager::run() {

    while (1) {
        ExportData data;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);

            while (result_queue.empty() && !stop_flag) {
                cv.wait(lock);
            }

            if (result_queue.empty() && stop_flag)   break;

            data = std::move(result_queue.front());
            result_queue.pop();

            Metrics::measurement().export_pending.fetch_sub(1);
            Metrics::measurement().export_running.fetch_add(1);
        }


        bool exported = export_result_and_index(std::move(data));
        Metrics::measurement().export_running.fetch_sub(1);


        if (!exported)
            AsyncLogger::logger().error("ExportManager::run() - failed to export result and index");
        else 
            Metrics::measurement().export_finished.fetch_add(1);
    }
}

void 
ExportManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        stop_flag = true;
    }
    cv.notify_all();
}

void 
ExportManager::push_queue(ExportData&& data) {
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        result_queue.push(std::move(data));
        AsyncLogger::logger().debug("ExportManager::push_queue result_queue.push(std::move(data));");
        Metrics::measurement().export_pending.fetch_add(1);
    }
    
    cv.notify_one();
}


bool    //consume
ExportManager::export_result_and_index(ExportData&& data) {
    Paths scan_paths;
    uint64_t id = data.scan_id;

    // update index during exporting
    if (!scan_report(std::move(data), scan_paths.detail_path, scan_paths.summary_path))
        return false;
    AsyncLogger::logger().debug("ExportManager::export_result_and_index::scan_report; index size: " + std::to_string(index.index_size()));

    {
        std::lock_guard<std::mutex> lock(map_mtx);
        export_map.emplace(id, scan_paths);
    }

    time_t timestamp = std::time(nullptr);
    bool update_index = false;
    uint64_t latest_version_number = index.get_version_number();
    {   
        std::lock_guard<std::mutex> lock(map_mtx);
        if (current_index.index_version != latest_version_number) {
            update_index = true;
        }
    }
    Paths index_paths;
    if (update_index) {
        if (!index_report(latest_version_number, timestamp, index_paths.detail_path, index_paths.summary_path))
            return false;
    } 
    AsyncLogger::logger().debug("ExportManager::export_result_and_index::index_report; index size: " + std::to_string(index.index_size()));

    {
        std::lock_guard<std::mutex> lock(map_mtx);

        if (update_index) {
            current_index.update(latest_version_number, timestamp);
            index_map.emplace(current_index.index_version, index_paths);
        }
    }
    return true;
}


bool 
ExportManager::set_dir(std::string&& dir) {
    if (!formater::validate_outdir(dir))
        return false;
    this->export_dir = dir;
    this->index_dir = std::move(dir);
    return true;
}   


bool
ExportManager::scan_report(ExportData&& data, std::string& result_path, std::string& summary_path) {
    AsyncLogger::logger().debug("ExportManager::scan_report");

    if (!formater::validate_outdir(export_dir))    return false;

    Exporter exporter(data.scan_id, std::move(data.root_path), data.canceled, std::move(data.result), index);
    if (!exporter.set_export_dir(export_dir))
        return false;
    if (!exporter.export_result(result_path))
        return false;
    if (!exporter.export_summary(summary_path))
        return false;
        
    return true;
}

bool
ExportManager::index_report(uint64_t version_number, time_t timestamp, \
            std::string& detail_path, std::string& summary_path) const {

    if (!formater::validate_outdir(index_dir))    return false;

    IndexReporter reporter(index, version_number);
    
    if (!reporter.set_export_dir(index_dir))
        return false;
    if (!reporter.export_index_summary(summary_path, timestamp))
        return false;
    if (!reporter.export_index_detail(detail_path, timestamp))
        return false;

    return true;
}

// call scheduler.get_status in advance to ensure scan is finished; 
// in order to distinguish EXPORTED from EXPORTING 

// bool
// ExportManager::check_exported(uint64_t scan_id) {
//     std::lock_guard<std::mutex> lock(map_mtx);
//     std::unordered_map<uint64_t, Paths>::iterator it = export_map.find(scan_id);
//     if (it == export_map.end()) {
//         AsyncLogger::logger().debug("ExportManager::check_exported - scan_id " + std::to_string(scan_id) + " not in export_map; maybe exporting, or pending -> canceled");
//         return false;
//     }
//     return true;
// }

std::vector<bool>
ExportManager::check_exported(const std::vector<uint64_t>& scan_ids) {
    std::lock_guard<std::mutex> lock(map_mtx);

    std::vector<bool> results;
    results.reserve(scan_ids.size());
    
    for (size_t i=0; i<scan_ids.size(); ++i) {
        std::unordered_map<uint64_t, Paths>::iterator it = export_map.find(scan_ids[i]);
        if (it == export_map.end()) {
            // AsyncLogger::logger().debug("ExportManager::check_exported - scan_id " + std::to_string(scan_ids[i]) + " not in export_map; maybe exporting, or pending -> canceled");
            results.push_back(0);
        }
        else    results.push_back(1);
    }

    return results;
}


bool 
ExportManager::get_scan_result(uint64_t scan_id, \
    std::string& scan_detail_path, std::string& scan_summary_path) {

    std::lock_guard<std::mutex> lock(map_mtx);

    std::unordered_map<uint64_t, Paths>::iterator it = export_map.find(scan_id);
    if (it == export_map.end()) {
        AsyncLogger::logger().info("ExportManager::get_scan_result - scan_id not in export_map; maybe exporting");
        return false;
    }

    scan_detail_path = it->second.detail_path;
    scan_summary_path = it->second.summary_path;

    return true;
}


bool
ExportManager::get_newest_index(uint64_t& version_number, time_t& snapshot_timestamp) {

    std::lock_guard<std::mutex> lock(map_mtx);

    if (current_index.index_version == 0) {
        AsyncLogger::logger().info("ExportManager::check_index - no previous scans recorded, system index empty");
        return true;
    }

    snapshot_timestamp = current_index.latest_time;
    version_number = current_index.index_version;

    return true;        //always
}
