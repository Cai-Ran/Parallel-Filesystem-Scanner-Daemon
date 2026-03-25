#pragma once
#include <cstdint>
#include <functional>
#include "database_connection.h"
#include "db_types.h"


class ScanTable {
private:
    DatabaseConnection& db_;
    sqlite3_stmt* stmt_upsert_   = nullptr;
    sqlite3_stmt* stmt_finish_   = nullptr;
    sqlite3_stmt* stmt_get_data_ = nullptr;
    sqlite3_stmt* stmt_get_all_  = nullptr;
    sqlite3_stmt* stmt_get_max_id_  = nullptr;
    sqlite3_stmt* stmt_update_cnt_  = nullptr;
    sqlite3_stmt* stmt_get_count_   = nullptr;


public:

    explicit ScanTable(DatabaseConnection& db);
    ~ScanTable();

    void upsert      (const ScanTaskRow& row);
    void update_end  (uint64_t scan_id, uint64_t finish_time);
    void upsert_count(uint64_t scan_id);

    void get_data(uint64_t scan_id, ScanTaskRow& row, uint64_t& total_size,
                                int& file_cnt, int& dir_cnt, int& link_cnt);
    void get_all(int limit, int offset, std::function<void(const ScanTaskRow&)>fn);
    uint64_t get_max_id();

};