#pragma once
#include <manager.h>
#include <scheduler.h>
#include <httpserver.h>
#include <submit_scan_result.h>
#include <config.h>

#include <vector>
#include <thread>
#include <atomic>
#include <chrono>


class Daemon {

private:
    Manager manager;
    Scheduler scheduler;
    HttpServer httpserver;
    static std::atomic<bool> shutdown_flag;
    static void terminate_handler(int sig_num);

    uint16_t USER_DOWNLOAD_SEC;


public:
    Daemon();
    void run();

    // httpserver api
    SubmitScanResult submit_scan(std::string&& root_path, uint64_t& scan_id);     //return id
    // RequestState get_state(uint64_t scan_id);
    std::vector<RequestState> get_state(const std::vector<uint64_t>& scan_ids);
    bool cancel_scan(uint64_t scan_id);
    void shutdown();

    // bool check_exported(uint64_t scan_id);
    std::vector<bool> check_exported(const std::vector<uint64_t>& scan_ids);


};