#pragma once
#include "database_connection.h"
#include "file_event.h"
#include "db_types.h"
#include <cstdint>


class IndexWriter {
private:
    DatabaseConnection& db_;
    sqlite3_stmt* stmt_compare_bf_upsert_           = nullptr;
    sqlite3_stmt* stmt_upsert_history_              = nullptr;
    sqlite3_stmt* stmt_upsert_current_              = nullptr;
    sqlite3_stmt* stmt_mark_deleted_history_        = nullptr;
    sqlite3_stmt* stmt_delete_current_              = nullptr;

public:
    explicit IndexWriter(DatabaseConnection& db);
    ~IndexWriter();

    void begin_transaction();
    void end_transaction();
    
    void upsert(const FileEvent& row);
    void mark_deleted(DeleteTask info);

    // utils
    std::string get_extension(const std::string& path);

};
