#pragma once
#include <string>
#include <stdexcept>
#include "database_connection.h"
#include "config.h"

class DatabaseInitialization {
    private:
        DatabaseConnection& db_;

    public:
        explicit DatabaseInitialization(DatabaseConnection& db) : db_(db){

            //WAL mode: write-write in order, read-read/read-write async
            std::string fsync_option = (Config::cfg().db().fsync) ? "NORMAL" : "OFF";
            db_.exec("PRAGMA journal_mode=WAL;");
            db_.exec("PRAGMA synchronous=" + fsync_option + ";");

            // includes all states COMAPRE FP
            db_.exec(R"(
                CREATE TABLE IF NOT EXISTS index_history (
                    last_scan_id    INTEGER,
                    path            TEXT,
                    node_type       INTEGER,
                    mtime           INTEGER,
                    msize           INTEGER,
                    state           INTEGER,
                    err_msg         TEXT,
                    extension       TEXT,
                    PRIMARY KEY (path, last_scan_id)
                );
            )");

            db_.exec("CREATE INDEX IF NOT EXISTS idx_scan_id ON index_history(last_scan_id);");
            
            // only alive / error state INSERT OR REPLACE
            db_.exec(R"(
                CREATE TABLE IF NOT EXISTS index_current (
                    scan_id         INTEGER,
                    path            TEXT    PRIMARY KEY,   
                    node_type       INTEGER,
                    mtime           INTEGER,
                    msize           INTEGER,
                    state           INTEGER,
                    err_msg         TEXT,
                    extension       TEXT
                );
            )");
            

            db_.exec(R"(
                CREATE TABLE IF NOT EXISTS scan_task_table (
                    scan_id         INTEGER PRIMARY KEY,
                    submit_root     TEXT,
                    start_time      INTEGER,
                    finish_time     INTEGER,
                    end_state       INTEGER,
                    dir_count       INTEGER,
                    file_count      INTEGER,
                    link_count      INTEGER,
                    total_size      INTEGER
                );
            )");

            db_.exec(R"(
                CREATE TABLE IF NOT EXISTS scan_diff_table (
                    scan_id         INTEGER,
                    path            TEXT,
                    diff_state      INTEGER,
                    old_size        INTEGER,
                    PRIMARY KEY (scan_id, path)
                );
            )");

        }

        ~DatabaseInitialization() {}

        //copy forbidden
        DatabaseInitialization& operator=(const DatabaseInitialization& db) = delete;       //DatabaseInitialization b = a;
        DatabaseInitialization(const DatabaseInitialization& db) = delete;    

};