#pragma once
#include <config.h>
#include <atomic>
#include <mutex>
#include <string>
#include <time.h>

#include <thread_pool.h>

class Daemon;

class HttpServer {
    private:
        Daemon& daemon;
        ThreadPool<int> fd_pool;
        
        uint16_t server_port;
        std::string frontend_path;

        time_t system_start_time;       //for reloading window, sync time

        static const size_t LIBHTTP_REQUEST_MAX_SIZE = 8192;
        std::atomic<bool> stop_flag{false};
        //protect server_socket
        std::mutex server_fd_mtx;       //setup_server_socket()/shutdown()
        int server_socket_fd = -1;      

        std::atomic<bool> drain_flag{false};
        std::atomic<bool> export_dir_set{false};

        struct RequestContent {
            std::string method;
            std::string path;
        };

        // http helpers
        bool setup_server_socket();
        bool setup_client_socket(int fd);
        RequestContent read_request(int fd);
        bool write_no_sigpipe(int fd, const std::string& data);
        bool write_response(int fd, int status_code, \
            const std::string& content_type, const std::string& body);
        
        
        // request handlers
        bool handle_request(int fd);
        int router(int fd, const RequestContent& request);

        // response handlers

