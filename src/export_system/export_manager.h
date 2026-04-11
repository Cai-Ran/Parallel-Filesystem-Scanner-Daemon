#pragma once

#include <ctime>
#include <cstdint>
#include <string>
#include <queue>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include "job_queue.h"
#include "db_types.h"
#include "index_writer.h"



class ExportManager {

private:

    std::string db_path;
    uint16_t WRITE_BATCH_SIZE;

    JobQueue<FileEvent>     result_queue;

    std::thread result_thread;

    bool stop_flag = false;

    void write_result_to_db();

    std::unordered_map<uint64_t, ScanTaskRow> pending_scan_rows;
    std::mutex pending_scan_rows_mtx;

    std::unordered_set<uint64_t> exported_map;
    std::mutex map_mtx;

    std::function<void(const std::string&)> notify_export_done;
    ScanTaskRow get_scan_task(const FileEvent& event);
    void finalize_scan(DatabaseConnection& db_con, const FileEvent& event, IndexWriter& writer);


public:
    ExportManager();
    ~ExportManager() {}

    // manager api
    bool start();
    void shutdown();


    void insert_scan_task(ScanTaskRow&& data);
    void update_scan_finish(uint64_t scan_id, time_t finish_time);
    void push_result_queue(FileEvent&& event);
      
    
    // bool check_exported(uint64_t scan_id);
    std::vector<bool> check_exported(const std::vector<uint64_t>& scan_ids);
    void set_scheduler_callback(std::function<void(const std::string&)> fn);
};
