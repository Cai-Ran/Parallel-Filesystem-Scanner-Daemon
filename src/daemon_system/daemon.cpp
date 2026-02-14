#include <daemon.h>
#include <async_logger.h>
#include <metrics.h>
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

