#include "scan_diff.h"



ScanDiff::ScanDiff(DatabaseConnection& db) : db_(db) {

    sqlite3_prepare_v2(
        db_.get(),
        "INSERT INTO scan_diff_table VALUES (?,?,?,?)",
        -1,
        &stmt_upsert_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "SELECT "
        "   COUNT(CASE WHEN diff_state = 0 THEN 1 END), "
        "   COUNT(CASE WHEN diff_state = 1 THEN 1 END), "
        "   COUNT(CASE WHEN diff_state = 2 THEN 1 END), "
        "   COUNT(CASE WHEN diff_state = 3 THEN 1 END), "
        "   COUNT(CASE WHEN diff_state = 4 THEN 1 END)  "
        "FROM scan_diff_table "
        "WHERE scan_id = ? ",
        -1, 
        &stmt_get_count_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),

        "SELECT d.diff_state, d.old_size, i.path, i.node_type, i.msize, i.mtime, i.err_msg " 
        "FROM scan_diff_table d "
        "JOIN index_history i ON d.path = i.path AND d.scan_id = i.last_scan_id "
        "WHERE d.scan_id = ? "
        "AND (?2 = -1 OR d.diff_state = ?2) "
        "LIMIT ? OFFSET ? ",

        -1,
        &stmt_get_detail_,
        nullptr
    );
}


ScanDiff::~ScanDiff() { 
    sqlite3_finalize(stmt_upsert_); 
    sqlite3_finalize(stmt_get_detail_);
    sqlite3_finalize(stmt_get_count_);
}


void 
ScanDiff::begin_transaction() {   db_.exec("BEGIN;");}

void 
ScanDiff::end_transaction() {    db_.exec("COMMIT;");}


void 
ScanDiff::upsert(int64_t scan_id, std::string&& path, int diff_state, int64_t old_size) {

    // db_.exec("BEGIN;");

    sqlite3_bind_int64  (stmt_upsert_, 1, scan_id);
    sqlite3_bind_text   (stmt_upsert_, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (stmt_upsert_, 3, diff_state);
    sqlite3_bind_int64  (stmt_upsert_, 4, old_size);

    sqlite3_step (stmt_upsert_);
    sqlite3_reset(stmt_upsert_);

    // db_.exec("COMMIT;");
}


// state_filter: ScanState + -1, cast outside
void 
ScanDiff::get_scan_diff_detail(uint64_t scan_id, int state_filter, int limit, int offset,
                                std::function<void(const ScanRow&, uint64_t)> fn) 
{
    sqlite3_bind_int64  (stmt_get_detail_, 1, scan_id);
    sqlite3_bind_int    (stmt_get_detail_, 2, state_filter);
    sqlite3_bind_int    (stmt_get_detail_, 3, limit);
    sqlite3_bind_int    (stmt_get_detail_, 4, offset);
    // OFFSET = (page - 1) * limit

    while (sqlite3_step(stmt_get_detail_) == SQLITE_ROW) {
        ScanRow row;

        uint64_t old_size = 0;
        row.state =     static_cast<ScanState>  (sqlite3_column_int     (stmt_get_detail_, 0));
        old_size =      static_cast<uint64_t>   (sqlite3_column_int64   (stmt_get_detail_, 1));
        row.path =      (const char*)            sqlite3_column_text    (stmt_get_detail_, 2);
        row.node_type = static_cast<NodeType>   (sqlite3_column_int     (stmt_get_detail_, 3));
        row.size =      static_cast<uint64_t>   (sqlite3_column_int64   (stmt_get_detail_, 4));
        row.modtime =   static_cast<uint64_t>   (sqlite3_column_int64   (stmt_get_detail_, 5));
        row.err =       (const char*)            sqlite3_column_text    (stmt_get_detail_, 6);

        fn(row, old_size);
    }

    sqlite3_reset(stmt_get_detail_);
}


void 
ScanDiff::get_scan_diff_count(uint64_t scan_id, int& new_cnt, 
                                int& err_cnt, int& deleted_cnt, 
                                int& cancel_cnt, int& changed_cnt) 
{
    sqlite3_bind_int64  (stmt_get_count_, 1, scan_id);

    if (sqlite3_step(stmt_get_count_) == SQLITE_ROW) {
        new_cnt     = sqlite3_column_int(stmt_get_count_, 0);
        err_cnt     = sqlite3_column_int(stmt_get_count_, 1);
        deleted_cnt = sqlite3_column_int(stmt_get_count_, 2);
        cancel_cnt  = sqlite3_column_int(stmt_get_count_, 3);
        changed_cnt = sqlite3_column_int(stmt_get_count_, 4);
    }

    sqlite3_reset(stmt_get_count_);
}
