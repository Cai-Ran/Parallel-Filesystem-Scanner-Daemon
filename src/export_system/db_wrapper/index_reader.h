#pragma once
#include <cstdint>
#include <functional>
#include "database_connection.h"
#include "db_types.h"
#include "scan_diff.h"



class IndexReader {
private:
    DatabaseConnection& db_;
    sqlite3_stmt* stmt_find_scan_diff_              = nullptr;
    sqlite3_stmt* stmt_group_by_folder_             = nullptr;
    sqlite3_stmt* stmt_group_by_extension_          = nullptr;
    sqlite3_stmt* stmt_search_                      = nullptr;
    sqlite3_stmt* stmt_get_top_root_                = nullptr;

public:
    explicit IndexReader(DatabaseConnection& db);
    ~IndexReader();


    void find_scan_diff_and_upsert_scandiff(uint64_t scan_id, ScanDiff& differ);

    void get_top_root_in_index(std::string& top_root);

    void group_by_folder_in_snd_layer(const std::string& root, 
        std::unordered_map<std::string, uint64_t>& msize_map);

    void group_by_extension(const std::string& root, 
        std::unordered_map<std::string, std::pair<int,uint64_t>>& msize_map);

    void search(std::string&& path, int limit, int offset,
        std::function<void(const IndexRow&)> fn);

    std::string get_snd_layer(const std::string& path, const std::string& root);
    std::string get_common_parent(const std::string& a, const std::string& b);
};
