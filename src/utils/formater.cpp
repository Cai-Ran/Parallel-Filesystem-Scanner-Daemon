#include <formater.h>
#include <async_logger.h>

#include <sys/stat.h>


namespace formater {

    bool validate_outdir(const std::string& path) {
        if (path.empty()) {
            AsyncLogger::logger().error("Error: directory is empty");
            return false;
        }
        struct stat stat_buff;
        if (lstat(path.c_str(), &stat_buff)!=0) {
            AsyncLogger::logger().error(std::system_error(errno, std::generic_category(), "path not exsist").what());
            return false;
        }
        if (!S_ISDIR(stat_buff.st_mode)) {
            AsyncLogger::logger().error("Error: path is not directory");
            return false;
        }
        return true;
    }

    bool validate_outpath(const std::string& path) {
        if (path.empty()) {
            AsyncLogger::logger().error("Error: directory is empty");
            return false;
        }
        struct stat stat_buff;
        if (lstat(path.c_str(), &stat_buff)!=0) {
            AsyncLogger::logger().error(std::system_error(errno, std::generic_category(), "path not exsist").what());
            return false;
        }
        if (!S_ISREG(stat_buff.st_mode)) {
            AsyncLogger::logger().error("Error: path is not directory");
            return false;
        }
        return true;
    }


    void transform_entry(Entry&& e, std::string&& path, JsonContent& json, EventState state) {

        std::string err_msg = "";

        json.path = std::move(path);
        json.node_type = e.node_type;
        json.size = e.fp.size;
        json.time = format_time(e.fp.modtime);
        json.state = state;
        json.err_msg = std::move(err_msg);
    }

    void transform_event(FileEvent&& e, JsonContent& json, EventState state) {    

        std::string err_msg = (state == EventState::Error) ? e.err.message() : "";

        json.path = std::move(e.path);
        json.node_type = e.node_type;
        json.size = e.size;
        json.time = format_time(e.modtime); //move assignment
        json.state = state;
        json.err_msg = std::move(err_msg);
    }

    std::string format_node(NodeType node_type) {

        std::string node = "";

        size_t type_idx = static_cast<size_t>(node_type);

        // static : initialized once for the entire program lifetime (not per function call)
        static const std::string type_names[] = {"unknown", "directory", "file", "link"};    
        size_t type_cnt = sizeof(type_names)/sizeof(type_names[0]);

        if (type_idx < type_cnt) 
            node = type_names[type_idx];

        return node;
    }

    std::string format_state(EventState state) {

        std::string s = "";

        size_t state_idx = static_cast<size_t>(state);

        static const std::string state_names[] = {"alive", "error", "deleted", "canceled"};
        size_t state_cnt = sizeof(state_names)/sizeof(state_names[0]);

        if (state_idx < state_cnt) 
            s = state_names[state_idx];

        return s;
    }


    std::string format_time(const time_t t) {
        struct tm tm{};
        localtime_r(&t, &tm);

        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);

        return std::string(buf);
    }

};

