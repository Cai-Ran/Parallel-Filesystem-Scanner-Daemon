#include <async_logger.h>
#include <config.h>
#include <metrics.h>

AsyncLogger* AsyncLogger::logger_ptr = nullptr;


bool 
AsyncLogger::start() {

    {
        std::lock_guard<std::mutex> lock(mtx);
        stop = false;
    }

    std::string dir = Config::cfg().asynclogger().log_dir;
    if (!formater::validate_outdir(dir)) {
        std::cerr << "Error: log directory is not found\n";
        log_path.clear();
    } else {
        std::string t = formater::format_time(std::time(nullptr));
        log_path = dir + "/log-" + t + ".log";
    }
    if (log_worker.joinable())      return false;             //avoid duplicate start operarion
    log_worker = std::thread(&AsyncLogger::worker_loop, this);
    return true;
}
