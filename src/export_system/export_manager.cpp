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
