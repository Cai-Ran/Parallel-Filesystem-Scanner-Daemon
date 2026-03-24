#pragma once
#include "database_connection.h"
#include "db_types.h"
#include <cstdint>
#include <functional>


class ScanDiff {
private: 
    DatabaseConnection& db_;
    sqlite3_stmt* stmt_upsert_                      = nullptr;
    sqlite3_stmt* stmt_get_count_                   = nullptr;
    sqlite3_stmt* stmt_get_detail_                  = nullptr;


public:
    explicit ScanDiff(DatabaseConnection& db);
    ~ScanDiff();

    void begin_transaction();
    void end_transaction();

    void upsert(int64_t scan_id, std::string&& path, int diff_state, int64_t old_size);

    // state_filter: ScanState + -1, cast outside
    void get_scan_diff_detail(uint64_t scan_id, int state_filter, int limit, int offset,
                                std::function<void(const ScanRow&, uint64_t)> fn); 
   
    void get_scan_diff_count(uint64_t scan_id, int& new_cnt, 
                                int& err_cnt, int& deleted_cnt, 
                                int& cancel_cnt, int& changed_cnt);

};