#include <manager.h>
#include <exporter.h>
#include <index_reporter.h>
#include <scheduler.h>
#include <async_logger.h>
#include <metrics.h>
#include <cstdlib>

namespace {
    JobQueue<ScanData>::QueueMetrics make_metrics() {
        JobQueue<ScanData>::QueueMetrics metrics{};
        metrics.submitted_total = &Metrics::measurement().scan_jobs_submitted;
        metrics.submitted_failed = &Metrics::measurement().scan_jobs_enqueue_reject;
        metrics.queued_number = &Metrics::measurement().scan_jobs_queued;
        return metrics;
    }
};

Manager::Manager()
    :scanner(*this), 
    scan_pool(JobQueue<ScanData>::QueueType::Lifo,
            make_metrics(),
            Config::cfg().manager().scan_queue_max_size,
            Config::cfg().manager().scan_pool_num_threads) 
    {
        Metrics::measurement().const_scan_job_pool_num_threads = Config::cfg().manager().scan_pool_num_threads;
        Metrics::measurement().const_scan_job_queue_size = Config::cfg().manager().scan_queue_max_size;
    };


void
Manager::start() {
    
    bool pool_success = scan_pool.start(
        [&scanner = this->scanner](ScanData data) { 
            scanner.worker_job(std::move(data));
        }
    );
    if (!pool_success)   return;


    try {
        export_manager_thread = std::thread(
            [&export_manager = this->export_manager]{
                export_manager.run();
            }
        );
    } catch (...) {
        AsyncLogger::logger().error("Manager::run - cannot start export_manager");
        scan_pool.shutdown();
        return;
    }

}


bool
Manager::start_new_scan(uint64_t scan_id, std::string root_path, bool& queue_full) {

    std::shared_ptr<ScanContext> ctx = std::make_shared<ScanContext>();
    queue_full = false;

    ScanData data(scan_id, root_path, ctx);

    {
        std::lock_guard<std::mutex> lock(registry_mtx);
        std::pair<std::unordered_map<uint64_t, ScanTask>::iterator, bool> res \
            = registry.emplace(scan_id, ScanTask(std::move(root_path)));

        if (!res.second) {
            AsyncLogger::logger().error("Manager::start_new_scan - scan already in map");
            return false;
        }
        res.first->second.context = ctx;
    }

    JobQueue<ScanData>::SubmitResult result = task_on_job_submit(data, true);
    
    if (result == JobQueue<ScanData>::SubmitResult::Shutdown) {
        AsyncLogger::logger().error("Manager::start_new_scan - start failed, system shutdown");    
        // unfinished_jobs sub is done on task_on_job_finish()
        // registry cleanup is handled by task_on_job_finish()
        return false;
    }

    if (result == JobQueue<ScanData>::SubmitResult::Full) {
        AsyncLogger::logger().error("Manager::start_new_scan - start failed, job queue full");
        data.context->unfinished_jobs.fetch_sub(1);
        Metrics::measurement().scan_jobs_unfinished.fetch_sub(1);
        dispatch_failed.store(true);
        queue_full = true;
        std::lock_guard<std::mutex> lock(registry_mtx);
        registry.erase(scan_id);
        return false;
    }

    return true;
}


void 
Manager::wait_to_finish(std::uint64_t scan_id) {
    
    std::unique_lock<std::mutex> lock(registry_mtx);
    // while (registry[scan_id].unfinished_jobs.load() != 0) 
        // manager_cv.wait(lock);
    manager_cv.wait(lock, 
        [scan_id, this] {
            std::unordered_map<uint64_t, ScanTask>::iterator it = registry.find(scan_id);
            return ((it == registry.end()) ||
                    (it->second.context->unfinished_jobs.load()==0));      //job is finished
        }
    );
    
}

void
Manager::set_scheduler_callback(std::function<void(uint64_t scan_id)> fn1,
                            std::function<void()> fn2) {
    notify_scan_finished = std::move(fn1);
    notify_dispatch_available = std::move(fn2);
}


bool 
Manager::clean_up(uint64_t scan_id) {
    {
        std::lock_guard<std::mutex> lock(registry_mtx);
        std::unordered_map<uint64_t, ScanTask>::iterator it = registry.find(scan_id);
        if (it != registry.end() && it->second.context->unfinished_jobs.load()==0) {
            registry.erase(it);
            return true;
        }
    }
    AsyncLogger::logger().error("Manager::clean_up - invalid clean up operation for manager registry");
    return false;
}


// ========================
// SCANNER API  
// ========================

JobQueue<ScanData>::SubmitResult
Manager::task_on_job_submit(ScanData& data, bool is_root) {

    data.context->unfinished_jobs.fetch_add(1);
    Metrics::measurement().scan_jobs_unfinished.fetch_add(1);

    std::shared_ptr<ScanContext> ctx = data.context;        //local variable will be destroyed automatically
    uint64_t id = data.scan_id;

    // set priority: root_path has higher priority
    size_t reserved = (Config::cfg().scheduler().max_concurrent_scan > 1) ? (Config::cfg().scheduler().max_concurrent_scan - 1) : 0u;
    size_t used_slots = scan_pool.jobs_in_queue();
    size_t free_slots = (Config::cfg().manager().scan_queue_max_size > used_slots) ? (Config::cfg().manager().scan_queue_max_size - used_slots) : 0u;
    bool enough = free_slots > reserved;
    if (!is_root && !enough)      return JobQueue<ScanData>::SubmitResult::Full;
    
    JobQueue<ScanData>::SubmitResult result = scan_pool.submit(std::move(data));
    //scanner will rollback scan_jobs_unfinished itself
    if (result == JobQueue<ScanData>::SubmitResult::Shutdown) {
        task_on_job_finish(id, ctx);
    } 

    return result;
}

