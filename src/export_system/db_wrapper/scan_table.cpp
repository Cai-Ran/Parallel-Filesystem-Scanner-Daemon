#include "scan_table.h"
#include "async_logger.h"

namespace {
    bool step_write_done(sqlite3_stmt* stmt, sqlite3* db, const char* op_name) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) return true;

        AsyncLogger::logger().error(
            std::string(op_name) +
            " sqlite3_step failed rc=" + std::to_string(rc) +
            " err=" + std::string(sqlite3_errstr(rc)) +
            " msg=" + std::string(sqlite3_errmsg(db))
        );
        return false;
    }
}

ScanTable::ScanTable(DatabaseConnection& db) : db_(db) {

    sqlite3_prepare_v2(
        db_.get(),
        "INSERT INTO scan_task_table VALUES (?,?,?,?,?,NULL,NULL,NULL,NULL)",
        -1,
        &stmt_upsert_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "UPDATE scan_task_table "
        "SET dir_count = ?, file_count = ?, link_count = ?, total_size = ? "
        "WHERE scan_id = ?",
        -1,
        &stmt_update_cnt_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "UPDATE scan_task_table "
        "SET finish_time = ? "
        "WHERE scan_id = ?",
        -1, 
        &stmt_finish_,
        nullptr
    );


    sqlite3_prepare_v2(
        db_.get(),
        "SELECT * FROM scan_task_table "
        "WHERE scan_id = ?",
        -1, 
        &stmt_get_data_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "SELECT * FROM scan_task_table "
        "ORDER BY scan_id DESC "
        "LIMIT ? OFFSET ? ",
        -1, 
        &stmt_get_all_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "SELECT MAX(scan_id) AS max_scan_id "
        "FROM scan_task_table ",
        -1, 
        &stmt_get_max_id_,
        nullptr
    );

    //must perform after mark_deleted
    sqlite3_prepare_v2(
        db_.get(),
        "SELECT "
        "    COUNT(CASE WHEN node_type = 1 THEN 1 END), "
        "    COUNT(CASE WHEN node_type = 2 THEN 1 END), "
        "    COUNT(CASE WHEN node_type = 3 THEN 1 END), "
        "    SUM(msize) "
        "FROM index_current "
        "WHERE scan_id = ? ",
        -1,
        &stmt_get_count_,
        nullptr
    );
};

ScanTable::~ScanTable() {
    sqlite3_finalize(stmt_upsert_);
    sqlite3_finalize(stmt_finish_);
    sqlite3_finalize(stmt_get_data_);
    sqlite3_finalize(stmt_get_all_);
    sqlite3_finalize(stmt_get_max_id_);
    sqlite3_finalize(stmt_get_count_);
    sqlite3_finalize(stmt_update_cnt_);
}

void 
ScanTable::upsert(const ScanTaskRow& row) {
    db_.exec("BEGIN;");
    
    sqlite3_bind_int64  (stmt_upsert_, 1, static_cast<int64_t>(row.scan_id));
    sqlite3_bind_text   (stmt_upsert_, 2, row.submit_root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64  (stmt_upsert_, 3, static_cast<int64_t>(row.start_time));
    sqlite3_bind_int64  (stmt_upsert_, 4, static_cast<int64_t>(row.finish_time));
    sqlite3_bind_int    (stmt_upsert_, 5, static_cast<int>    (row.end_state));

    step_write_done(stmt_upsert_, db_.get(), "ScanTable::upsert");
    sqlite3_reset(stmt_upsert_);

    db_.exec("COMMIT;");
};

//unused
void 
ScanTable::update_end(uint64_t scan_id, uint64_t finish_time) {
    db_.exec("BEGIN;");
    
    sqlite3_bind_int64  (stmt_finish_, 2, static_cast<int64_t>(scan_id));
    sqlite3_bind_int64  (stmt_finish_, 1, static_cast<int64_t>(finish_time));

    step_write_done(stmt_finish_, db_.get(), "ScanTable::update_end");
    sqlite3_reset(stmt_finish_);

    db_.exec("COMMIT;");
};

void 
ScanTable::get_data(uint64_t scan_id, ScanTaskRow& row, uint64_t& total_size,
    int& file_cnt, int& dir_cnt, int& link_cnt) {

    sqlite3_bind_int64  (stmt_get_data_, 1, static_cast<int64_t>(scan_id));

    if (sqlite3_step(stmt_get_data_) == SQLITE_ROW) {
        row.scan_id     =   static_cast<uint64_t>(sqlite3_column_int64(stmt_get_data_, 0));
        row.submit_root =            (const char*)sqlite3_column_text (stmt_get_data_, 1);
        row.start_time  =   static_cast<uint64_t>(sqlite3_column_int64(stmt_get_data_, 2));
        row.finish_time =   static_cast<uint64_t>(sqlite3_column_int64(stmt_get_data_, 3));
        row.end_state  =static_cast<RequestState>(sqlite3_column_int  (stmt_get_data_, 4));
        dir_cnt         =                        (sqlite3_column_int  (stmt_get_data_, 5));
        file_cnt        =                        (sqlite3_column_int  (stmt_get_data_, 6));
        link_cnt        =                        (sqlite3_column_int  (stmt_get_data_, 7));
        total_size      =   static_cast<uint64_t>(sqlite3_column_int64(stmt_get_data_, 8));
    }

    
    sqlite3_reset(stmt_get_data_);
};


void 
ScanTable::get_all(int limit, int offset, std::function<void(const ScanTaskRow&)>fn) {

    sqlite3_bind_int    (stmt_get_all_, 1, limit);
    sqlite3_bind_int    (stmt_get_all_, 2, offset);
    // OFFSET = (page - 1) * limit

    while (sqlite3_step(stmt_get_all_) == SQLITE_ROW) {
        ScanTaskRow row;

        row.scan_id     = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_all_, 0));
        row.submit_root =          (const char*)sqlite3_column_text (stmt_get_all_, 1);
        row.start_time  = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_all_, 2));
        row.finish_time = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_all_, 3));
        row.end_state=static_cast<RequestState>(sqlite3_column_int  (stmt_get_all_, 4));

        fn(row);
    }
    
    sqlite3_reset(stmt_get_all_);
}


uint64_t
ScanTable::get_max_id() {

    uint64_t max_scan_id = 0;

    if (sqlite3_step(stmt_get_max_id_) == SQLITE_ROW) 
        max_scan_id = static_cast<uint64_t>(sqlite3_column_int64(stmt_get_max_id_, 0));
    
    sqlite3_reset(stmt_get_max_id_);

    return max_scan_id;
};


void 
ScanTable::upsert_count(uint64_t scan_id) {
    db_.exec("BEGIN IMMEDIATE;");   // force fresh snapshot + exclusive write
    sqlite3_bind_int64  (stmt_get_count_, 1, scan_id);

    int dir_count = 0, file_count = 0, link_count = 0;
    int64_t total_size = 0;
    
    if (sqlite3_step(stmt_get_count_) == SQLITE_ROW) {
        dir_count  = (sqlite3_column_int64(stmt_get_count_, 0));
        file_count = (sqlite3_column_int64(stmt_get_count_, 1));
        link_count = (sqlite3_column_int64(stmt_get_count_, 2));
        total_size = (sqlite3_column_int64(stmt_get_count_, 3));
    }
    sqlite3_reset(stmt_get_count_);

    AsyncLogger::logger().debug(
        "upsert_count scan_id=" + std::to_string(scan_id) +
        " dir=" + std::to_string(dir_count) +
        " file=" + std::to_string(file_count) +
        " link=" + std::to_string(link_count)
    );


    sqlite3_bind_int    (stmt_update_cnt_, 1, dir_count);
    sqlite3_bind_int    (stmt_update_cnt_, 2, file_count);
    sqlite3_bind_int    (stmt_update_cnt_, 3, link_count);
    sqlite3_bind_int64  (stmt_update_cnt_, 4, total_size);
    sqlite3_bind_int64  (stmt_update_cnt_, 5, scan_id);

    step_write_done(stmt_update_cnt_, db_.get(), "ScanTable::upsert_count");
    sqlite3_reset(stmt_update_cnt_);

    db_.exec("COMMIT;");

}
