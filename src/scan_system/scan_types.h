#pragma once
#include <file_event.h>
#include <async_logger.h>

#include <string>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>

// necessary context for scan worker
struct ScanContext {
    std::atomic<bool> canceled{false};          //Manager write; Scanner read    
    std::atomic<int> unfinished_jobs{0};        //Scanner write
};


struct ScanTask {
    //metadata; manager write
    std::string root_path;                      //metadata; manager write
    time_t start_time = 0;
    // time_t finish_time = 0;                  //straightly record to db 
    std::shared_ptr<ScanContext> context;

    ScanTask(std::string root): root_path(root), start_time(std::time(nullptr)),context(){};
};


struct ScanData {
    uint64_t scan_id;            
    std::string path;                           //job target
    std::shared_ptr<ScanContext> context;       //write result in shared map for specific scan_id
    ScanData():scan_id(0) {};
    ScanData(uint64_t id, std::string p, std::shared_ptr<ScanContext> ctx)
        : scan_id(id), path(std::move(p)), context(std::move(ctx)) {}; 
        //new job; copy ScanContext once (copy argument + std::move avoid twice)
};



