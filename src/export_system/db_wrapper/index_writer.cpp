#include "index_writer.h"
#include "async_logger.h"
#include <thread>
#include <chrono>

namespace {
    bool step_write_done(sqlite3_stmt* stmt, sqlite3* db, const char* op_name, int max_retries = 3) {
        int rc = SQLITE_ERROR;
        for (int i = 0; i <= max_retries; ++i) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) return true;
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
        }

        AsyncLogger::logger().error(
            std::string(op_name) +
            " sqlite3_step failed rc=" + std::to_string(rc) +
            " ext_rc=" + std::to_string(sqlite3_extended_errcode(db)) +
            " err=" + std::string(sqlite3_errstr(rc)) +
            " msg=" + std::string(sqlite3_errmsg(db))
        );
        return false;
    }
}

IndexWriter::IndexWriter(DatabaseConnection& db) : db_(db) {

    sqlite3_prepare_v2(
        db_.get(),
        "SELECT mtime, msize FROM index_current "
        "WHERE path = ? ",
        -1,
        &stmt_compare_bf_upsert_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "INSERT INTO index_history VALUES (?,?,?,?,?,?,?,?)",
        -1,
        &stmt_upsert_history_,
        nullptr
    );

    //  Correlated Subquery O(K log N)
    // NOTE:
    // Do not use scan_id ordering here. scan_id is submit order, while DB writes follow
    // completion order under concurrency. We only want "rows under root not touched by
    // current scan", i.e. scan_id != current scan_id.
    sqlite3_prepare_v2(
        db_.get(),
        "INSERT INTO index_history "
        "(last_scan_id, path, node_type, mtime, msize, state, err_msg, extension) "
        "SELECT ?,      path, node_type, mtime, msize, ?,     ''     , extension  "
        "FROM index_current "
        "WHERE path GLOB ? "
        "AND scan_id != ? ",
        -1,
        &stmt_mark_deleted_history_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "INSERT OR REPLACE INTO index_current "
        "(scan_id, path, node_type, mtime, msize, state, err_msg, extension) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) ",
        -1, 
        &stmt_upsert_current_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "DELETE FROM index_current "
        "WHERE path GLOB ? "
        "AND scan_id != ? ",
        -1,
        &stmt_delete_current_,
        nullptr
    );

}


IndexWriter::~IndexWriter() {
    sqlite3_finalize(stmt_compare_bf_upsert_);
    sqlite3_finalize(stmt_upsert_history_);
    sqlite3_finalize(stmt_upsert_current_);
    sqlite3_finalize(stmt_mark_deleted_history_);
    sqlite3_finalize(stmt_delete_current_);
}


void 
IndexWriter::begin_transaction() {   db_.exec("BEGIN IMMEDIATE;");}

void 
IndexWriter::end_transaction() {    db_.exec("COMMIT;");}

void 
IndexWriter::upsert(const FileEvent& row) {

    // db_.exec("BEGIN;");

    /* index current */

    //1. compare fingerprint from index_current
    sqlite3_bind_text(stmt_compare_bf_upsert_, 1, row.path.c_str(), -1, SQLITE_TRANSIENT);

    int64_t latest_mtime = -1;
    int64_t latest_msize = -1;

    if (sqlite3_step(stmt_compare_bf_upsert_) == SQLITE_ROW) {
        latest_mtime = sqlite3_column_int64(stmt_compare_bf_upsert_, 0);
        latest_msize = sqlite3_column_int64(stmt_compare_bf_upsert_, 1);
    }
    sqlite3_reset(stmt_compare_bf_upsert_);


    //2. update index current 
    EventState state = (row.err.value() == 0) ? EventState::Alive : EventState::Error;
    std::string err_msg = (row.err.value() == 0) ? "" : row.err.message();
    std::string extension = get_extension(row.path);

    sqlite3_bind_int64  (stmt_upsert_current_, 1, static_cast<int64_t>(row.scan_id));
    sqlite3_bind_text   (stmt_upsert_current_, 2, row.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (stmt_upsert_current_, 3, static_cast<int>(row.node_type));
    sqlite3_bind_int64  (stmt_upsert_current_, 4, static_cast<int64_t>(row.modtime));
    sqlite3_bind_int64  (stmt_upsert_current_, 5, static_cast<int64_t>(row.size));
    sqlite3_bind_int    (stmt_upsert_current_, 6, static_cast<int>(state));
    sqlite3_bind_text   (stmt_upsert_current_, 7, err_msg.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text   (stmt_upsert_current_, 8, extension.c_str(), -1, SQLITE_TRANSIENT);

    step_write_done(stmt_upsert_current_, db_.get(), "IndexWriter::upsert index_current");
    sqlite3_reset(stmt_upsert_current_);


    /* index history */
    //3. insert (append only) into index_history

    // if fp not changed -> skip

    if (
        static_cast<int64_t>(row.modtime) == latest_mtime &&
        static_cast<int64_t>(row.size) == latest_msize) 
    {
        // db_.exec("ROLLBACK;");
        return;
    }

    sqlite3_bind_int64  (stmt_upsert_history_, 1, static_cast<int64_t>(row.scan_id));
    sqlite3_bind_text   (stmt_upsert_history_, 2, row.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (stmt_upsert_history_, 3, static_cast<int>(row.node_type));
    sqlite3_bind_int64  (stmt_upsert_history_, 4, static_cast<int64_t>(row.modtime));
    sqlite3_bind_int64  (stmt_upsert_history_, 5, static_cast<int64_t>(row.size));
    sqlite3_bind_int    (stmt_upsert_history_, 6, static_cast<int>(state));
    sqlite3_bind_text   (stmt_upsert_history_, 7, err_msg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text   (stmt_upsert_history_, 8, extension.c_str(), -1, SQLITE_TRANSIENT);

    step_write_done(stmt_upsert_history_, db_.get(), "IndexWriter::upsert index_history");
    sqlite3_reset(stmt_upsert_history_);

    // db_.exec("COMMIT;");
}


// Inserts Deleted rows for paths under root_path not seen in this scan.
void 
IndexWriter::mark_deleted(DeleteTask info) {

    std::string glob_pattern = info.submit_root + "/*";

    db_.exec("BEGIN;");

    // INSERT deleted rows into index_history

    EventState state = (info.canceled) ? EventState::Canceled : EventState::Deleted;

    sqlite3_bind_int64  (stmt_mark_deleted_history_, 1, static_cast<int64_t>(info.scan_id));
    sqlite3_bind_int    (stmt_mark_deleted_history_, 2, static_cast<int>(state));
    sqlite3_bind_text   (stmt_mark_deleted_history_, 3, glob_pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64  (stmt_mark_deleted_history_, 4, static_cast<int64_t>(info.scan_id));

    step_write_done(stmt_mark_deleted_history_, db_.get(), "IndexWriter::mark_deleted insert_deleted_history");
    sqlite3_reset(stmt_mark_deleted_history_);

    // For canceled scans we must keep index_current unchanged as the baseline,
    // otherwise the next full scan will falsely report "Changed" for paths
    // that were only missing due to cancellation.
    if (!info.canceled) {
        // DELETE unvisited paths from index_current
        sqlite3_bind_text   (stmt_delete_current_, 1, glob_pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64  (stmt_delete_current_, 2, static_cast<int64_t>(info.scan_id));

        step_write_done(stmt_delete_current_, db_.get(), "IndexWriter::mark_deleted delete_current");
        sqlite3_reset(stmt_delete_current_);
    }

    db_.exec("COMMIT;");
    int changed = sqlite3_changes(db_.get());
    AsyncLogger::logger().debug("mark_deleted scan_id=" + std::to_string(info.scan_id) 
        + " deleted=" + std::to_string(changed));


}

// ===========
// UTILS
// ===========

std::string 
IndexWriter::get_extension(const std::string& path) {

    size_t pos = path.rfind('.');
    if (pos == std::string::npos) return "";
    return path.substr(pos + 1);
}
