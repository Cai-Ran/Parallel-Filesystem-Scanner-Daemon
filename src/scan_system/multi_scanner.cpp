#include <multi_scanner.h>
#include <manager.h>
#include <async_logger.h>

#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>



// =============================
// MULTI THREAD SCAN
// =============================

MultiScanner::MultiScanner(Manager& m)
    :manager(m){}


void
MultiScanner::worker_job(const ScanData& job_data) {
    if (job_data.context->canceled.load()) {
        manager.task_on_job_finish(job_data.scan_id, job_data.context);   
        return;
    }
    // to make sure any exception will be digested here, job must finish
    try {
        std::vector<ScanData> childrens = scan_one_node(job_data);

        for (size_t i=childrens.size(); i-->0;) {
            if (childrens[i].context->canceled.load())
                break;

            ScanData child = std::move(childrens[i]);
            JobQueue<ScanData>::SubmitResult result = manager.task_on_job_submit(child, false);
            if (result == JobQueue<ScanData>::SubmitResult::Shutdown)
                break;
            if (result == JobQueue<ScanData>::SubmitResult::Full)
                worker_job(child);          //fallback
                
        } 
    } catch (...) {
        AsyncLogger::logger().error("unknown error at MultiScanner::worker_job");
    }
    // make sure job finish
    manager.task_on_job_finish(job_data.scan_id, job_data.context);
}


std::vector<ScanData>
MultiScanner::scan_one_node(const ScanData& data) {

    std::vector<ScanData> childrens;

    FileEvent event;
    const std::string& path = data.path;
    event.path = path;


    struct stat stat_buff;
    if (lstat(path.c_str(), &stat_buff) != 0) {          //handle dynamic path change error
        event.err = std::error_code(errno, std::generic_category());
        event.node_type = NodeType::UNKNOWN;
        data.context->result.record(std::move(event));
        return childrens;
    }

    if (S_ISLNK(stat_buff.st_mode)) {                  //skip symlink to avoid infinite loop
        event.err = {};
        event.node_type = NodeType::LINK;
        event.modtime = stat_buff.st_mtime;
        event.size = stat_buff.st_size;
        data.context->result.record(std::move(event));
        return childrens; 
    }


    if (!S_ISDIR(stat_buff.st_mode)) {  //is file 
        event.node_type = NodeType::FILE;
        event.modtime = stat_buff.st_mtime;
        event.size = stat_buff.st_size;
        event.err = {};                //success
        data.context->result.record(std::move(event));
        return childrens;
    }


    //is dir -> DFS
    event.node_type = NodeType::DIR;

    // check open permission
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        event.err = std::error_code(errno, std::generic_category());
        data.context->result.record(std::move(event));
        return childrens;
    }

    // check read permission
    struct dirent* entry;
    errno = 0;      //clear old error status
    entry = readdir(dir);
    if (!entry && errno != 0) {
        event.err = std::error_code(errno, std::generic_category());
        data.context->result.record(std::move(event));
        closedir(dir);
        return childrens;
    }

    event.modtime = stat_buff.st_mtime;
    event.size = stat_buff.st_size;
    event.err = {};
    data.context->result.record(std::move(event));

    while (entry) {
        std::string name = entry->d_name;

        if ( name == "." || name == "..") {
            entry = readdir(dir);
            continue;
        }

        std::string p = path + "/" + name;
        ScanData new_data(data.scan_id, std::move(p), data.context);

        childrens.push_back(std::move(new_data));

        entry = readdir(dir);
    }

    closedir(dir);
    return childrens;
};
