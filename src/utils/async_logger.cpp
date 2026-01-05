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


// single thread worker
void 
AsyncLogger::worker_loop() {
    std::ostream* out = &std::cerr;
    std::ofstream file;

    if (!log_path.empty()) {
        file.open(log_path, std::ios::out | std::ios::app);     //write; append
        if (file.is_open()) 
            out = &file;
        else
            std::cerr << "[Error] Logger cannot open file; fallback to stderr\n";
    }


    LogItem job;
    while (consume(job)) {      //always true until (empty && shutdown)
        write_log(*out, job);
    }

}
// single thread worker
void 
AsyncLogger::write_log(std::ostream& os, const LogItem& job) {

    static const std::string arr[4] = {"Info", "Warn", "Debug", "Error"};
    os << "[" << arr[job.level] << "]" << " ";
    os << "[TID" << (std::hash<std::thread::id>{}(job.tid) % 10000) << "] ";
    os << formater::format_time(job.timestamp) << " ";
    os << job.msg << " \n";

    os.flush();
    if (!os.good()) {
        std::cerr << "log flush error";
    }
    Metrics::measurement().logger_finished.fetch_add(1);
}

void 
AsyncLogger::print_to_terminal(LogItem item) {

    // to avoid broken / unordered output
    std::lock_guard<std::mutex> lock(cerr_mtx);

    static const std::string arr[4] = {"Info", "Warn", "Debug", "Error"};

    std::cerr << "[" << arr[item.level] << "]" 
              << "[TID" << (std::hash<std::thread::id>{}(item.tid) % 10000) << "]"
              << " " << formater::format_time(item.timestamp) 
              << " " << item.msg << " \n";

    Metrics::measurement().logger_fallback.fetch_add(1);
}
