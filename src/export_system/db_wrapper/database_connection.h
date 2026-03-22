#pragma once 
#include <string>
#include <stdexcept>
#include <sqlite3.h>
#include "config.h"

class DatabaseConnection {
private:
    sqlite3* db_ = nullptr;

public:
    void exec(const std::string& command) {
        char* err = nullptr;
        if (sqlite3_exec(db_, command.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err;
            sqlite3_free(err);       //inside api it malloc err
            throw std::runtime_error(msg);
        }
    }

    sqlite3* get() {
        return db_;
    }

    explicit DatabaseConnection(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));

        // for write-write conflict (sqlite only allow serial write)
        sqlite3_busy_timeout(db_, 5000);    //wait for 5000ms and retry

        exec("PRAGMA foreign_keys=ON;");
    }
    //explicit to avoid DatabaseConnection db = "path";

    ~DatabaseConnection() {
        sqlite3_close(db_);
    }

    //copy forbidden
    DatabaseConnection& operator=(const DatabaseConnection& db) = delete;       //DatabaseConnection b = a;
    DatabaseConnection(const DatabaseConnection& db) = delete;                  //DatabaseConnection b(a);
};
