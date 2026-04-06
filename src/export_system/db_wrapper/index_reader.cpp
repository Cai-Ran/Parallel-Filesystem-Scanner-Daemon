#include "index_reader.h"



IndexReader::IndexReader(DatabaseConnection& db) : db_(db){


    sqlite3_prepare_v2(
        db_.get(),
        "SELECT " 
        "    i.path,"
        "    i.state,"
        "    (SELECT p.msize "
        "       FROM index_history p "
        "       WHERE p.path = i.path "
        "       AND p.last_scan_id < ?1 "
        "       AND p.state != ?2 "
        "       ORDER BY p.last_scan_id DESC LIMIT 1) AS prev_msize,"
        "    (SELECT p.state "
        "       FROM index_history p " 
        "       WHERE p.path = i.path "
        "       AND p.last_scan_id < ?1 "
        "       AND p.state != ?2 "
        "       ORDER BY p.last_scan_id DESC LIMIT 1) AS prev_state "
        "FROM index_history i "
        "WHERE i.last_scan_id = ?1",
        -1,
        &stmt_find_scan_diff_,
        nullptr
    );


    sqlite3_prepare_v2(
        db_.get(),
        "SELECT path, msize, node_type "
        "FROM index_current "
        "WHERE path GLOB ? ",
        -1,
        &stmt_group_by_folder_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),
        "SELECT extension, COUNT(*), SUM(msize) "
        "FROM index_current "
        "WHERE path GLOB ? "
        "AND node_type = ? "
        "GROUP BY extension",
        -1,
        &stmt_group_by_extension_,
        nullptr
    );


    sqlite3_prepare_v2(
        db_.get(),

        "SELECT path, msize, mtime, node_type, state, err_msg "
        "FROM index_current "
        "WHERE path LIKE ? "
        "LIMIT ? OFFSET ? ",

        -1,
        &stmt_search_,
        nullptr
    );

    sqlite3_prepare_v2(
        db_.get(),

        "SELECT MIN(path), MAX(path) "
        "FROM index_current "
        "WHERE path > '' ",

        -1,
        &stmt_get_top_root_,
        nullptr
    );
    

    /*  get scan_detail_:

        1. list condition:
            path under scan_submit_root
        &&  last_scan_id <= scan_id

        2. get row with max(last_scan_id)  under condition
        [last_scan_id = (SELECT MAX ... <= ?)]
    */
}

IndexReader::~IndexReader() {
    sqlite3_finalize(stmt_find_scan_diff_);
    sqlite3_finalize(stmt_group_by_folder_);        
    sqlite3_finalize(stmt_group_by_extension_);         
    sqlite3_finalize(stmt_search_);       
    sqlite3_finalize(stmt_get_top_root_);                   
}


void 
IndexReader::find_scan_diff_and_upsert_scandiff(uint64_t scan_id, ScanDiff& differ) {
    sqlite3_bind_int64(stmt_find_scan_diff_, 1, scan_id);
    sqlite3_bind_int64(stmt_find_scan_diff_, 2, static_cast<int>(EventState::Canceled));

    differ.begin_transaction();

    while (sqlite3_step(stmt_find_scan_diff_) == SQLITE_ROW) {

        ScanRow row; 

        row.path =              (const char*)sqlite3_column_text    (stmt_find_scan_diff_, 0);       //const unsigned char* -> const char*
        row.state =   static_cast<ScanState>(sqlite3_column_int     (stmt_find_scan_diff_, 1));
        int has_prev =                       sqlite3_column_type    (stmt_find_scan_diff_, 3);
            
        uint64_t old_msize = 0;
        ScanState old_state = ScanState::Canceled;   // dummy

        if (has_prev != SQLITE_NULL) {
            old_msize= static_cast<uint64_t>  (sqlite3_column_int64   (stmt_find_scan_diff_, 2));
            old_state= static_cast<ScanState> (sqlite3_column_int     (stmt_find_scan_diff_, 3));
        }

        if (row.state == ScanState::New) {
            row.state = (old_state == ScanState::Deleted || has_prev == SQLITE_NULL) ? ScanState::New : ScanState::Changed;
        }
        /*
        old     curr
        Alive   Alive  -> changed
        Deleted Alive  -> new
        NULL    Alive  -> new
        old Canceled is skipped
        */

        differ.upsert(scan_id, std::move(row.path), static_cast<int>(row.state), old_msize);
    }

    differ.end_transaction();

    sqlite3_reset(stmt_find_scan_diff_);
}


void 
IndexReader::get_top_root_in_index(std::string& top_root) {

    const char *min_ptr = nullptr, *max_ptr = nullptr;

    if (sqlite3_step(stmt_get_top_root_) == SQLITE_ROW) {
        min_ptr =    (const char*)sqlite3_column_text (stmt_get_top_root_, 0);
        max_ptr =    (const char*)sqlite3_column_text (stmt_get_top_root_, 1);
    }

    if (!min_ptr || !max_ptr)   
        top_root = "";

    else 
        top_root = get_common_parent(min_ptr, max_ptr);
        

    sqlite3_reset(stmt_get_top_root_);
}

void 
IndexReader::group_by_folder_in_snd_layer(const std::string& root, 
    std::unordered_map<std::string, uint64_t>& msize_map) {
    
    std::string glob_pattern = root + "/*";

    sqlite3_bind_text   (stmt_group_by_folder_, 1, glob_pattern.c_str(), -1, SQLITE_TRANSIENT);
    
    while (sqlite3_step(stmt_group_by_folder_) == SQLITE_ROW) {
        
        NodeType nodetype =     static_cast<NodeType>(sqlite3_column_int  (stmt_group_by_folder_, 2));

        std::string path  =              (const char*)sqlite3_column_text (stmt_group_by_folder_, 0);
        std::string snd_layer = get_snd_layer(path, root);
        uint64_t msize = static_cast<uint64_t>(sqlite3_column_int64(stmt_group_by_folder_, 1));
        if (path == snd_layer && nodetype != NodeType::DIR)          continue;
        
        msize_map[snd_layer] += msize;
    }

    sqlite3_reset(stmt_group_by_folder_);
}

void 
IndexReader::group_by_extension(const std::string& root, 
    std::unordered_map<std::string, std::pair<int,uint64_t>>& msize_map) 
{
    std::string glob_pattern = root + "/*";

    sqlite3_bind_text   (stmt_group_by_extension_, 1, glob_pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (stmt_group_by_extension_, 2, static_cast<int>(NodeType::FILE));

    while (sqlite3_step(stmt_group_by_extension_) == SQLITE_ROW) {
        // extension, COUNT(*), SUM(msize) 
        std::string extension =    (const char*)sqlite3_column_text (stmt_group_by_extension_, 0);
        int count             =                 sqlite3_column_int  (stmt_group_by_extension_, 1);
        uint64_t total_size   =                 sqlite3_column_int64(stmt_group_by_extension_, 2);

        msize_map[extension] = {count, total_size};
    }

    sqlite3_reset(stmt_group_by_extension_);
}

void 
IndexReader::search(std::string&& path, int limit, int offset,
    std::function<void(const IndexRow&)> fn) 
{
    std::string normalized = std::move(path);
    if (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
    if (normalized.empty())     return;
    // std::string glob_pattern = normalized + "/*";
    std::string like_pattern = "%" + normalized + "%";

    sqlite3_bind_text   (stmt_search_, 1, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (stmt_search_, 2, limit);
    sqlite3_bind_int    (stmt_search_, 3, offset);

    while (sqlite3_step(stmt_search_) == SQLITE_ROW) {
        IndexRow row;

        row.path =      (const char*)            sqlite3_column_text    (stmt_search_, 0);
        row.size =      static_cast<uint64_t>   (sqlite3_column_int64   (stmt_search_, 1));
        row.modtime =   static_cast<uint64_t>   (sqlite3_column_int64   (stmt_search_, 2));
        row.node_type = static_cast<NodeType>   (sqlite3_column_int     (stmt_search_, 3));
        row.state =     static_cast<EventState> (sqlite3_column_int     (stmt_search_, 4));
        row.err =       (const char*)            sqlite3_column_text    (stmt_search_, 5);

        fn(row);
    }

    sqlite3_reset(stmt_search_);
};


// ========
// UTILS 
// ========

std::string 
IndexReader::get_snd_layer(const std::string& path, const std::string& root) {
    std::string after_root = path.substr(root.size() + 1);
    size_t pos = after_root.find('/');
    if (pos == std::string::npos) return path;
    return root + "/" + after_root.substr(0, pos);
};

std::string 
IndexReader::get_common_parent(const std::string& a, const std::string& b) {
    
    const size_t n = (a.size() > b.size()) ? b.size() : a.size();
    size_t i = 0;

    // /abc/cc
    // /abc/cccf
    //       ^
    while (i < n && a[i] == b[i])   ++i;
    if (i == 0)                     return "";

    // /abc/cc
    //       ^
    // /abc/c/
    const bool boundary = 
        (i == a.size() || a[i] == '/') &&
        (i == b.size() || b[i] == '/');

    std::string prefix = a.substr(0, i);

    // /abc/cc
    // /abc/cccf
    //     ^
    if (!boundary) {
        size_t cut = prefix.find_last_of('/');
        if (cut == std::string::npos)   return "";
        prefix.resize(cut);
    }

    if (prefix.size() > 1 && prefix.back() == '/')  prefix.pop_back();

    return prefix;
}
