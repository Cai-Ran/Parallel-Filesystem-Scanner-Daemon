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


