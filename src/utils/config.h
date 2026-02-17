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



class Config {
    private:
        static Config* config_ptr;

    public:
        Config() {};
        static void switch_config(Config& ins) {
            config_ptr = &ins;
        }
        static Config& cfg() {
            static Config global_cfg;        //HERE create global config
            if (!config_ptr)    config_ptr = &global_cfg;
            return *config_ptr;
        };

    private:
        // copy forbidden
        Config(const Config&) = delete;                     //Config b = a;
        Config& operator=(const Config& other) = delete;    // b = a;
        // move forbidden
        Config(Config&&) = delete;                    //Config b = std::move(a);  
        Config& operator=(Config&& other) = delete;   //b = std::move(a);

        DaemonConfig        daemon_;
        HttpServerConfig    httpserver_;
        SchedulerConfig     scheduler_;
        AsyncLoggerConfig   asynclogger_;
        ExportManagerConfig exportmanager_;
        ManagerConfig       manager_;

        template <typename F, typename T>
        T clamp(F old, T new_min, T new_max) {
            if (old < static_cast<F>(new_min))   return new_min;
            if (old > static_cast<F>(new_max))   return new_max;
            return static_cast<T>(old);
        }

    public:
        bool load(const std::string& file_path);

        const DaemonConfig&
            daemon() const {        return daemon_; }
        const HttpServerConfig& 
            httpserver() const {    return httpserver_; }
        const SchedulerConfig& 
            scheduler() const {     return scheduler_; }
        const AsyncLoggerConfig&
            asynclogger() const {    return asynclogger_; }
        const ExportManagerConfig&
            exportmanager() const { return exportmanager_; }
        const ManagerConfig&
            manager() const { return manager_; }
};

