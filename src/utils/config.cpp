#include "config.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <limits>

Config* Config::config_ptr = nullptr;

// remove " " \t \r \n
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool 
Config::load(const std::string& config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "[Config] Unable to open file: " << config_file << std::endl;
        return false;
    }

    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        // remove comments (# or ;)
        size_t comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) line = line.substr(0, comment_pos);
        line = trim(line);
        if (line.empty()) continue;

        // section header: [section]
        if (line.front() == '[' && line.back() == ']') {
            section = to_lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        // parse Key = Value
        size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) continue;

        std::string key = to_lower(trim(line.substr(0, equal_pos)));
        std::string value = trim(line.substr(equal_pos + 1));

        // remove ""
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        // support old "section.key" style if no [section] is set
        std::string sec = section;
        if (sec.empty()) {
            size_t dot = key.find('.');
            if (dot != std::string::npos) {
                sec = key.substr(0, dot);
                key = key.substr(dot + 1);
            }
        }

        // map to struct 
        try {
            if (sec == "daemon") {
                if (key == "user_download_sec")    
                    daemon_.user_download_sec = clamp(stoi(value), std::numeric_limits<uint16_t>::min(), std::numeric_limits<uint16_t>::max());
            } else if (sec == "httpserver") {
                if (key == "server_port")   
                    httpserver_.server_port = clamp(stoi(value), std::numeric_limits<uint16_t>::min(), std::numeric_limits<uint16_t>::max());
                else if (key == "frontend_path")  httpserver_.frontend_path = value;
                else if (key == "fd_queue_max_size" || key == "queue_max_size")         
                    httpserver_.fd_queue_max_size = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
                else if (key == "fd_pool_num_threads" || key == "pool_num_threads")     
                    httpserver_.fd_pool_num_threads = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
            } else if (sec == "scheduler") {
                if (key == "max_concurrent_scan")                                 
                    scheduler_.max_concurrent_scan = (stoi(value) > 0) ? stoi(value) : 1;
                else if (key == "queue_max_size" || key == "pending_queue_max_size")
                    scheduler_.queue_max_size = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
            } else if (sec == "asynclogger" || sec == "async_logger") {
                if (key == "log_dir")                                                   asynclogger_.log_dir = value;
                else if (key == "queue_max_size")                                       
                    asynclogger_.queue_max_size = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
            } else if (sec == "exportmanager" || sec == "export_manager") {
                if (key == "export_dir" || key == "result_dir")                         exportmanager_.export_dir = value;
                else if (key == "index_dir")                                            exportmanager_.index_dir = value;
            } else if (sec == "manager") {
                if (key == "scan_queue_max_size" || key == "queue_max_size")            
                    manager_.scan_queue_max_size = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
                else if (key == "scan_pool_num_threads" || key == "pool_num_threads")   
                    manager_.scan_pool_num_threads = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
            } else if (sec == "threadpool") {
                if (key == "num_threads" || key == "pool_num_threads") {
                    const size_t n = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
                    manager_.scan_pool_num_threads = n;
                    httpserver_.fd_pool_num_threads = n;
                }
            } else if (sec == "jobqueue") {
                if (key == "max_size" || key == "queue_max_size") {
                    const size_t n = clamp(std::stoull(value), std::numeric_limits<size_t>::min(), std::numeric_limits<size_t>::max());
                    manager_.scan_queue_max_size = n;
                    httpserver_.fd_queue_max_size = n;
                }
            }
        } catch (...) {
            std::cerr << "[Config] Invalid value for key: " << key << std::endl;
        }
    }
    return true;
}

