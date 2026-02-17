#pragma once
#include <cstdint>
#include <string>

struct DaemonConfig {
    uint16_t user_download_sec = 300;
};

// =======================
// thread pool + job queue
// =======================

struct HttpServerConfig {
    uint16_t server_port = 8080;
    std::string frontend_path = "./web/index.html";
    size_t fd_queue_max_size = 1024;
    size_t fd_pool_num_threads = 4;
};

struct ManagerConfig {
    size_t scan_queue_max_size = 1024;
    size_t scan_pool_num_threads = 4;
};

// ==============================
// multi producer + one consumer
// ==============================

struct SchedulerConfig {
    int max_concurrent_scan = 3;
    size_t queue_max_size = 64;
};

struct AsyncLoggerConfig {
    std::string log_dir;
    size_t queue_max_size = 8192;
};

struct ExportManagerConfig {
    std::string export_dir;
    std::string index_dir;
};



