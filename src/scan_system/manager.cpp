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

