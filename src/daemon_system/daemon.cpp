#include "daemon.h"
#include "async_logger.h"
#include "database_connection.h"
#include "database_initialization.h"
#include "scan_table.h"
#include "metrics.h"
#include <csignal>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <iostream>


std::atomic<bool> Daemon::shutdown_flag{false};

Daemon::Daemon()
    :manager(), scheduler(manager), httpserver(*this),
    USER_DOWNLOAD_SEC(Config::cfg().daemon().user_download_sec)
{
    manager.set_scheduler_callback(
        [this](uint64_t id) {                               //this: access ServiceRuntime member
            scheduler.notify_scan_finished(id);             //(): accept argument
        },
        [this]() {
            scheduler.notify_dispatch_available();
        },
        [this](const std::string& root) {
            scheduler.notify_export_done(root);
        }
    );
};


// SIG handler: for SIGINT, SIGTERM
void 
Daemon::terminate_handler(int sig_num) {
    (void) sig_num;             //avoid unused warning

    //debug
    const char msg[] = "SIGINT/SIGTERM received\n";
    ::write(STDERR_FILENO, msg, sizeof(msg) - 1);  // signal handler-safe


    shutdown_flag.store(true);
};


void 
Daemon::run() {
    shutdown_flag.store(false);

    std::signal(SIGINT, &terminate_handler);        //register SIG handler
    std::signal(SIGTERM, &terminate_handler);       //register SIG handler

    
    DatabaseConnection      db_con(Config::cfg().db().db_path);
    DatabaseInitialization  init_db(db_con);

    manager.start();

    ScanTable scan_table_reader(db_con);
    scheduler.init_history_scan_id(scan_table_reader.get_max_id());

    std::thread t_scheduler(&Scheduler::run, &scheduler);
    std::thread t_httpserver(&HttpServer::run, &httpserver);


    while (!shutdown_flag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    //system shutting down
    httpserver.drain();

    scheduler.shutdown();   //calls manager.shutdown();
    
    int drain_time = 1000;
    bool server_stopped = false;

    while (drain_time--) {
        if (
            Metrics::measurement().scan_running.load() == 0         &&
            Metrics::measurement().scan_pending.load() == 0         &&
            Metrics::measurement().scan_jobs_unfinished.load() == 0 &&
            Metrics::measurement().export_result_pending.load() == 0&&
            Metrics::measurement().export_result_running.load() == 0&&
            Metrics::measurement().export_delete_pending.load() == 0&&
            Metrics::measurement().export_delete_running.load() == 0
        )
        {
            
            std::this_thread::sleep_for(std::chrono::seconds(USER_DOWNLOAD_SEC));
            std::cerr << "==================================\n"
                            "httpserver is closing...\n"
                         "==================================\n";
            httpserver.shutdown();
            server_stopped = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    if (!server_stopped) {
        httpserver.shutdown();
        AsyncLogger::logger().error("metrics value not drained; stop httpserver unexpectedly.");
    }

    if (t_scheduler.joinable())     t_scheduler.join();
    if (t_httpserver.joinable())    t_httpserver.join();
}



SubmitScanResult
Daemon::submit_scan(std::string&& root_path, uint64_t& scan_id) {
    scan_id = 0;    //invalid id; to avoid missed initialization
    return scheduler.submit_scan_root(std::move(root_path), scan_id);
}

std::vector<RequestState>
Daemon::get_state(const std::vector<uint64_t>& scan_ids) {
    return scheduler.get_state(scan_ids);
}

std::vector<bool> 
Daemon::check_exported(const std::vector<uint64_t>& scan_ids) {
    return manager.check_exported(scan_ids);
}

bool 
Daemon::cancel_scan(uint64_t scan_id) {
    bool canceled = scheduler.cancel(scan_id);
    // if (canceled)   manager.clean_up(scan_id);       //only erase after export
    return canceled;
}

void
Daemon::shutdown() {
    shutdown_flag.store(true);
}


