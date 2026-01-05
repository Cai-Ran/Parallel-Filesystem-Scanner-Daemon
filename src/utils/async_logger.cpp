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

// multi thread push
void 
AsyncLogger::helper(LogLevel level, const std::string& msg) {
    LogItem item;
    item.msg = msg;
    item.level = level;
    item.timestamp = std::time(nullptr);
    item.tid = std::this_thread::get_id();

    bool to_terminal = false;

    {
        std::unique_lock<std::mutex> lock(mtx);

        while (log_queue.size() >= MAX_QUEUE_SIZE && !stop) {
            if (level == LogLevel::Info || level == LogLevel::Debug) {
                to_terminal = true;
                break;
            }     
            //blocking wait  
            if (level == LogLevel::Warn || level == LogLevel::Error) {
                cv.wait(lock);
            }
        }

        if (stop) {
            to_terminal = true;
        } 
        if (!to_terminal) {
            log_queue.push(std::move(item));
            Metrics::measurement().logger_pending.fetch_add(1);
        }
    }

    if (to_terminal) {
        print_to_terminal(item);
        return;
    }
    
    cv.notify_one();

}


//single thread pop
bool 
AsyncLogger::consume(LogItem& job) {
    {
        std::unique_lock<std::mutex> lock(mtx);

        while (log_queue.empty() && !stop)
            cv.wait(lock);

        if (log_queue.empty() && stop)
            return false;
        
        job = std::move(log_queue.front());
        log_queue.pop();
        
        Metrics::measurement().logger_pending.fetch_sub(1);
    }
    cv.notify_all();

    return true;
}


void 
AsyncLogger::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (stop)       return;     //avoid duplicate shutdown due to destructor
        stop = true;
    }
    cv.notify_all();

    if (log_worker.joinable())
        log_worker.join();
}
