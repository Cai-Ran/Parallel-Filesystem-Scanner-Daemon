#pragma once

#include <ctime>
#include <cstdint>
#include <string>
#include <queue>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>

#include <file_event.h>
#include <metadata_index.h>
#include <config.h>
#include <async_logger.h>


// result_queue: multiple producer (scanner call task_on_job_finish) + one consumer (one thread for IO; serial IO (run))
// export_map: one writer (run) + multiple reader (httpserver)
// unbounded queue, definitely accept results transfer from manager registry map
// (memory usage is limited by upstream manager <- scheduler)

class ExportManager {

public:
    struct ExportData {
        uint64_t scan_id = 0;
        std::string root_path;
        bool canceled = false;
        std::vector<FileEvent> result;
    };


private:
    MetadataIndex index;

    std::string export_dir;
    std::string index_dir;

    bool stop_flag = false;

    struct Paths {
        std::string detail_path;
        std::string summary_path; 
    };

    struct IndexLatestState {
        uint64_t index_version;
        time_t latest_time;

        IndexLatestState():index_version(0), latest_time(0) {};

        void update(uint64_t version, time_t timestamp) {
            index_version = version;
            latest_time = timestamp;
        }
    };
    
    // map for recording result path
    std::unordered_map<uint64_t, Paths> export_map;     //key: scan_id
    std::unordered_map<uint64_t, Paths> index_map;      //key: index_version
    IndexLatestState current_index;
    std::mutex map_mtx;

    std::queue<ExportData> result_queue;
    std::mutex queue_mtx;
    std::condition_variable cv;

    bool export_result_and_index(ExportData&& data);
    bool scan_report(ExportData&& data, std::string& result_path, std::string& summary_path);
    bool index_report(uint64_t version_number, time_t timestamp, \
                        std::string& detail_path, std::string& summary_path) const;

