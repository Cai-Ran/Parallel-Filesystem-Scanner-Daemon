#pragma once

#include <metadata_index.h>
#include <formater.h>
#include <stats.h>
#include <async_logger.h>

#include <vector>

class Exporter {

private:
    uint64_t scan_id;
    const std::string root_path;
    const bool is_canceled;
    std::vector<FileEvent> scan_collection;
    std::unordered_map<std::string, FileEvent> scan_result;
    MetadataIndex& index;
    std::string out_dir;

    std::unordered_map<std::string, bool> seen_with_update;
    std::unordered_map<std::string, Entry> deleted_map;

    //collect for summary
    ScanStats scan_summary;
    bool summary_collected = false;

    void convert_to_map();


    void check_deleted();
    void update_index();
    
    bool is_under_root(const std::string& root_path, const std::string& path) const;    

    void make_summary(ScanStats& summary, const JsonContent& c) const; 



public:
    Exporter(uint64_t scan_id, std::string root_path, bool is_canceled,
        std::vector<FileEvent>&& result, MetadataIndex& index);

    ~Exporter() {
        AsyncLogger::logger().debug("~Exporter destruct");
        scan_result = std::unordered_map<std::string, FileEvent>{};
        seen_with_update = std::unordered_map<std::string, bool>{};
        deleted_map = std::unordered_map<std::string, Entry>{};
        scan_collection = std::vector<FileEvent>{};
    }

    bool set_export_dir(const std::string& dir);

    bool export_result(std::string& file_path); 
    bool export_summary(std::string& file_path);


};
