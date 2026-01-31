#pragma once
#include <scan_task.h>
#include <string>
#include <cstdint>
#include <memory>

struct ScanData {
    uint64_t scan_id;            
    std::string path;                           //job target
    std::shared_ptr<ScanContext> context;       //write result in shared map for specific scan_id
    ScanData():scan_id(0) {};
    ScanData(uint64_t id, std::string p, std::shared_ptr<ScanContext> ctx)
        : scan_id(id), path(std::move(p)), context(std::move(ctx)) {}; 
        //new job; copy ScanContext once (copy argument + std::move avoid twice)
};

