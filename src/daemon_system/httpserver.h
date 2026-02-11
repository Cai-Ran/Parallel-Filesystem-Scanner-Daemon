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
        int response_homepage(int fd);                                      
        int response_export_dir(int fd, const std::string& queries);        
        int response_state(int fd, const std::string& queries);             
        int response_exporting(int fd, const std::string& queries);
        int response_scan(int fd, const std::string& queries);              
        int response_cancel(int fd, const std::string& queries);          
        int response_shutdown();                                            
        int response_metrics(int fd);                                       
        int response_export_summary(int fd, const std::string& queries);     //return path to front end
        int response_export_detail(int fd, const std::string& queries);      //return path json
        int response_index(int fd);                                          //return id, timestamp to frontend
        int response_index_summary(int fd, const std::string& queries);      //return path to front end
        int response_index_detail(int fd, const std::string& queries);       //return path json
        int response_download_time(int fd);

        // utils
        static bool get_query_value(const std::string& queries, \
                const std::string& key, std::string& value);
        static int char_to_num(char c);
        bool parse_id(const std::string& id_str, uint64_t& id);
        bool serve_page(std::string file_path, std::string& body_buf);
        bool assemble_html(uint64_t id, const std::string& detail_route, \
                const std::string& summary_json, std::string& html_buf);

        // export state
        enum ExportState {
            Unavailable,
            Exporting,
            Exported
        };

    
    public:
        HttpServer(Daemon& daemon_);
        void run();
        void shutdown();
        void drain();

};

