#include <multi_scanner.h>
#include <manager.h>
#include <async_logger.h>

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>



// =============================
// MULTI THREAD SCAN
// =============================

MultiScanner::MultiScanner(Manager& m)
    :manager(m){}


void
MultiScanner::worker_job(const ScanData& job_data) {
    if (job_data.context->canceled.load()) {
        manager.task_on_job_finish(job_data.scan_id, job_data.context);   
        return;
    }
    // to make sure any exception will be digested here, job must finish
    try {
        std::vector<ScanData> childrens = scan_one_node(job_data);

        for (size_t i=childrens.size(); i-->0;) {
            if (childrens[i].context->canceled.load())
                break;

            ScanData child = std::move(childrens[i]);
            JobQueue<ScanData>::SubmitResult result = manager.task_on_job_submit(child, false);
            if (result == JobQueue<ScanData>::SubmitResult::Shutdown)
                break;
            if (result == JobQueue<ScanData>::SubmitResult::Full)
                worker_job(child);          //fallback
                
        } 
    } catch (...) {
        AsyncLogger::logger().error("unknown error at MultiScanner::worker_job");
    }
    // make sure job finish
    manager.task_on_job_finish(job_data.scan_id, job_data.context);
}
