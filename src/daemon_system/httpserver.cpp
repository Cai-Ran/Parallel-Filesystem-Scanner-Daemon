#include <httpserver.h>
#include <daemon.h>
#include <async_logger.h>
#include <request_state.h>
#include <submit_scan_result.h>


#include <netinet/in.h>     //sockaddr_in, htons, ntohs
#include <arpa/inet.h>      //inet_ntoa
#include <sys/socket.h>
#include <sys/time.h>       //struct timeval

#include <unistd.h>         //::read, ::send
#include <cerrno>
#include <string>
#include <thread>
#include <fstream>         
#include <sstream>
#include <vector>


const size_t HttpServer::LIBHTTP_REQUEST_MAX_SIZE;

namespace {
    JobQueue<int>::QueueMetrics make_metrics() {
        JobQueue<int>::QueueMetrics metrics{};
        metrics.submitted_total =  &Metrics::measurement().request_jobs_submitted;
        metrics.submitted_failed = &Metrics::measurement().request_jobs_failed;
        metrics.queued_number =    &Metrics::measurement().request_jobs_queued;
        return metrics;
    }
};


HttpServer::HttpServer(Daemon& daemon_) 
    :daemon(daemon_), 
    fd_pool(    JobQueue<int>::QueueType::Fifo,
                make_metrics(),
                Config::cfg().httpserver().fd_queue_max_size,
                Config::cfg().httpserver().fd_pool_num_threads),
    server_port(Config::cfg().httpserver().server_port), 
    frontend_path(Config::cfg().httpserver().frontend_path),
    system_start_time(time(nullptr))
    {
        Metrics::measurement().const_request_pool_num_threads = Config::cfg().httpserver().fd_pool_num_threads;
        Metrics::measurement().const_request_queue_size = Config::cfg().httpserver().fd_queue_max_size;
    };


bool
HttpServer::setup_server_socket() {

    // Creates a socket for IPv4 and TCP
    int fd = ::socket(PF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        AsyncLogger::logger().error("Failed to create server socket");
        return false;
    }

    // Allow the server to reuse the same port immediately after restart.
    // If setsockopt returns -1, the call failed.
    int socket_option = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &socket_option, sizeof(socket_option)) == -1) {
        AsyncLogger::logger().error("Failed to set socket options");
        ::close(fd);
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;           //IPv4 family
    server_addr.sin_addr.s_addr = INADDR_ANY;   //bind (0.0.0.0)
    server_addr.sin_port = htons(server_port);  //convert port to network byte order??ig endian??

    // sockaddr is the generic socket address type (socket.h)
    // sockaddr_in is the IPv4-specific version. cast to sockaddr type
    int binded = ::bind(fd, (sockaddr*)&server_addr, sizeof(server_addr));
    if (binded == -1) {
        AsyncLogger::logger().error("Failed to bind connection");
        ::close(fd);
        return false;
    }

    int listend = ::listen(fd, 1024);
    if (listend == -1) {
        AsyncLogger::logger().error("Failed to listen to server");
        ::close(fd);
        return false;
    }

    // avoid race: shutdown while setting up
    {
        std::lock_guard<std::mutex> lock(server_fd_mtx);

        if (stop_flag.load()) {
            ::close(fd);
            return false;
        }
        server_socket_fd = fd;
    }


    std::cout << "Listening on port " << server_port << std::endl;

    return true;
};


void 
HttpServer::run() {
    drain_flag.store(false);
    export_dir_set.store(false);

    if (!setup_server_socket())    return;  

    bool pool_success = fd_pool.start(
        [this](int client_fd){handle_request(std::move(client_fd));}
    );
    if (!pool_success)              return;


    while (!stop_flag.load()) {
        sockaddr_in client_addr{};
        socklen_t client_addr_size = sizeof(client_addr);

        int server_fd = -1;
        {
            std::lock_guard<std::mutex> lock(server_fd_mtx);
            server_fd = server_socket_fd;
        }

        if (server_fd == -1) {      
            if (stop_flag.load())   break;
            continue;
        }

        int client_socket_fd = ::accept(server_fd, (sockaddr*)&client_addr, &client_addr_size);

        if (client_socket_fd == -1) {       //race: if shutdown happened after server_fd_mtx
            if (stop_flag.load())  break;   //shutdown situation exclude by stop_flag
            AsyncLogger::logger().error("Failed to accept socket");
            continue;
        }

        if (!setup_client_socket(client_socket_fd)) 
            continue;

        // std::cout << "Accepting connection from " << ::inet_ntoa(client_addr.sin_addr) 
        //     << " on port " <<  ntohs(client_addr.sin_port) << std::endl;

        JobQueue<int>::SubmitResult result = fd_pool.submit(std::move(client_socket_fd));

        if (result == JobQueue<int>::SubmitResult::Full) {
            std::string body = "{\"error\": \"Server busy, please submit later\" }";
            write_response(client_socket_fd, 429, "application/json", body);
            ::close(client_socket_fd);
        } else if (result == JobQueue<int>::SubmitResult::Shutdown) {
            write_response(client_socket_fd, 503, "text/html", "");
            ::close(client_socket_fd);
        }

    }

    fd_pool.shutdown();
}


void 
HttpServer::shutdown() {
    
    int fd = -1;


    //get server_fd under mutex protection
    {
        std::lock_guard<std::mutex> lock(server_fd_mtx);
        stop_flag.store(true);
        
        fd = server_socket_fd;
        server_socket_fd = -1;
    }

    if (fd != -1) {
        ::shutdown(fd, SHUT_RDWR);                  // stop read / stop write
        ::close(fd);                                // close fd
    }
}


void 
HttpServer::drain() {
    drain_flag.store(true);
}


// ==================
// HTTP helpers
// ==================

bool
HttpServer::setup_client_socket(int fd) {
    timeval tv{};
    tv.tv_sec = 10;     //timeout: 10s + 0us 
    tv.tv_usec = 0;     
    // SOL_SOCKET: set option at "socket" layer 
    // SO_RCVTIMEO: maximum blocking time for recv/read 
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
        int err = errno;
        AsyncLogger::logger().error("setsockopt(SO_RCVTIMEO) failed, errno=" + std::to_string(err));
        ::close(fd);
        return false;
    }
    return true;
}


HttpServer::RequestContent
HttpServer::read_request(int fd) {
    char read_buf[LIBHTTP_REQUEST_MAX_SIZE];
    int bytes_read = ::read(fd, read_buf, LIBHTTP_REQUEST_MAX_SIZE-1);
    if (bytes_read < 1)             return {};
    read_buf[bytes_read] = '\0';

    RequestContent content;

    std::string line = std::string(read_buf);

    //parse method
    std::string method = "";
    for (size_t i=0; i<line.size(); ++i) {
        if (line[i] >= 'A' && line[i] <= 'Z')
            method.push_back(line[i]);
        else if (line[i] == ' ')    
            break;
    }
    if (method.size() == 0)         return {};
    content.method = std::move(method);

    // parse path
    if (line.size() < content.method.size() || line[content.method.size()] != ' ')
                                    return {};
    
    std::string path = "";
    for (size_t i=content.method.size()+1; i<line.size(); ++i) {
        if (line[i] != '\0' && line[i] != ' ' && line[i] != '\n' && line[i] != '\r')
            path.push_back(line[i]);
        else  break;
    }

    if (path.size() == 0)           return {};
    content.path = std::move(path);
    
    return content;
}

bool
HttpServer::write_no_sigpipe(int fd, const std::string& data) {
    // Loop until all bytes are sent; single send() can legally do partial writes.
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.c_str() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        // ignore client close to avoid kernel send SIGPIPE
        return false;
    }
    return true;
}


bool
HttpServer::write_response(int fd, int status_code, 
    const std::string& content_type, const std::string& body) {

    std::string response_msg = "";
    switch (status_code) {
        // case 100:        //client body
        //     response_msg = "Continue";          break;
        case 200:
            response_msg = "OK";                   break;
        case 204:
            response_msg = "No Content";           break;
        // case 301:        //redirect
        //     response_msg = "Moved Permanently"; break;
        // case 302:        //redirect
        //     response_msg = "Found";             break;
        // case 304:        //cache
        //     response_msg = "Not Modified";      break;
        case 400:
            response_msg = "Bad Request";          break;
        // case 403:        //auth
        //     response_msg = "Unauthorized";      break;
        case 404:
            response_msg = "Not Found";            break;
        // case 405:        //router defined
        //     response_msg = "Method Not Allowed";break;
        case 409:
            response_msg = "Conflict";              break;
        case 429:
            response_msg = "Server busy";           break;
        case 500:
            response_msg = "Internal Server Error"; break;
        case 503:
            response_msg = "Service Unavailable";   break;
        default:
            response_msg = "Internal Server Error";
    }


    bool success = true;
    std::string start_response = "HTTP/1.1 " + std::to_string(status_code)
                                    + " " + response_msg + "\r\n";
    if (!write_no_sigpipe(fd, start_response)) success = false;

    std::string header = "Content-Type: " + content_type + "\r\n"
                   + "Content-Length: " + std::to_string(body.size()) + "\r\n"
                   + "Connection: close\r\n\r\n";
    // HTTP/1.1 + Connection: close: browser will not attempt to reuse this connection,
    // preventing the keep-alive mismatch that caused 300ms connect times and connection pool saturation.


    if (!write_no_sigpipe(fd, header)) success = false;

    if (!body.empty()) 
        if (!write_no_sigpipe(fd, body)) success = false;
    

    if (!success)
        AsyncLogger::logger().error("write reponse failed");

    return success;
}


// ================
// Request Handlers
// ================ 

bool
HttpServer::handle_request(int fd) {

    RequestContent request = read_request(fd);

    if (request.method.empty() || request.path.empty() || request.path[0] != '/') {
        write_response(fd, 400, "text/html", "");
        AsyncLogger::logger().error("invalid request");
        ::close(fd);
        return false;
    }

    int status_code = router(fd, request);
    if (status_code != 0) 
        write_response(fd, status_code, "text/html", "");

    ::close(fd);
    return true;
}


int
HttpServer::router(int fd, const RequestContent& request) {

    bool dir_set = export_dir_set.load();
    if (!dir_set) {
        bool allow = 
            (request.method == "GET"  && request.path == "/")                             ||
            (request.method == "POST" && request.path.rfind("/export_dir", 0)       == 0) ||
            (request.method == "GET"  && request.path.rfind("/metrics", 0)          == 0) ||
            (request.method == "POST" && request.path.rfind("/shutdown", 0)         == 0) ||
            (request.method == "GET"  && request.path.rfind("/download_time", 0)    == 0);
        if (!allow) return 503;
    }

    bool in_drain = drain_flag.load();
    if (in_drain) {
        bool allow =
            (request.method == "GET" && request.path == "/")                             ||
            (request.method == "GET" && request.path.rfind("/metrics", 0)          == 0) ||
            (request.method == "GET" && request.path.rfind("/state", 0)            == 0) ||
            (request.method == "GET" && request.path.rfind("/exporting", 0)        == 0) ||
            (request.method == "GET" && request.path.rfind("/export_summary", 0)   == 0) ||
            (request.method == "GET" && request.path.rfind("/export_detail", 0)    == 0) ||
            (request.method == "GET" && request.path.rfind("/index_summary", 0)    == 0) ||
            (request.method == "GET" && request.path.rfind("/index_detail", 0)     == 0);
        if (!allow) return 503;
    }
    

    if (request.method == "GET" && request.path == "/") 
        return response_homepage(fd);
    if (request.method == "POST" && request.path.rfind("/shutdown", 0)      == 0) 
        return response_shutdown();
    if (request.method == "POST" && request.path.rfind("/index", 0)         == 0)
        return response_index(fd);
    if (request.method == "GET" && request.path.rfind("/metrics", 0)        == 0)
        return response_metrics(fd);
    if (request.method == "GET" && request.path.rfind("/download_time", 0)  == 0)
        return response_download_time(fd);
    

    size_t ques_pos = request.path.find('?');
    if (ques_pos == std::string::npos)      return 400;

    std::string queries = request.path.substr(ques_pos+1);
    if (queries.empty())                    return 400;

    if (request.method == "POST" && request.path.rfind("/export_dir", 0)    == 0)
        return response_export_dir(fd, queries);
    if (request.method == "POST" && request.path.rfind("/scan", 0)          == 0)
        return response_scan(fd, queries);
    if (request.method == "POST" && request.path.rfind("/cancel", 0)        == 0) 
        return response_cancel(fd, queries);
    if (request.method == "GET" && request.path.rfind("/state", 0)          == 0)
        return response_state(fd, queries);
    if (request.method == "GET" && request.path.rfind("/exporting", 0)      == 0)
        return response_exporting(fd, queries);
    if (request.method == "GET" && request.path.rfind("/export_summary", 0) == 0)
        return response_export_summary(fd, queries);
    if (request.method == "GET" && request.path.rfind("/export_detail", 0)  == 0)
        return response_export_detail(fd, queries);
    if (request.method == "GET" && request.path.rfind("/index_summary", 0)  == 0)
        return response_index_summary(fd, queries);
    if (request.method == "GET" && request.path.rfind("/index_detail", 0)   == 0)
        return response_index_detail(fd, queries);

    return 404;
}

