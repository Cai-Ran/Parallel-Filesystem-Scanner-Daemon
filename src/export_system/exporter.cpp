#include <exporter.h>
#include <async_logger.h>

#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <cassert>
#include <vector>
// #include <malloc.h>


Exporter::Exporter(uint64_t scan_id_, std::string root_path_, bool canceled,
    std::vector<FileEvent>&& result, MetadataIndex& index_)
    :scan_id(scan_id_), root_path(std::move(root_path_)), is_canceled(std::move(canceled)),
        scan_collection(std::move(result)) , index(index_) 
    {
        AsyncLogger::logger().debug("Exporter::Exporter - data moved to scan_collection vector");
    }


void 
Exporter::convert_to_map() {
     for (size_t i=0; i<scan_collection.size(); ++i) {
        std::string p = scan_collection[i].path;
        std::pair<std::unordered_map<std::string, FileEvent>::iterator, bool> res \
            = scan_result.emplace(std::move(p), std::move(scan_collection[i]));

        if (!res.second)
            AsyncLogger::logger().error("duplicated path in scan result");
     }
     //release mem of collection vector
     scan_collection = std::vector<FileEvent>{};
     AsyncLogger::logger().debug("Exporter::convert_to_map: scan_collection vector released; scan_collection vector convert to scan_result map;");
}


bool
Exporter::set_export_dir(const std::string& path) {
    if (!formater::validate_outdir(path))
        return false;

    out_dir = path;
    return true;
}


bool 
Exporter::is_under_root(const std::string& root_path, const std::string& path) const {
    if (path.size() < root_path.size())     return false;
    if (path.compare(0, root_path.size(), root_path) != 0) return false;
    if (path.size() == root_path.size())    return true;
    if (root_path[root_path.size()-1] != '/' && path[root_path.size()] != '/') return false;
    return true;
}


void
Exporter::check_deleted() {
    deleted_map.clear();
    seen_with_update.clear();
    index.iterate_entry([&](const std::string& path, const Entry& ent) 
        {
            if (!is_under_root(root_path, path)) return;
            std::unordered_map<std::string, FileEvent>::iterator it = scan_result.find(path);
            if ( it == scan_result.end()) {     //same root_path, in index but not in this scan result
                deleted_map.emplace(path, ent);
            } else {
                if (!ent.fp.match(it->second) && it->second.err.value() == 0)  
                //in index also in this scan, check if modified
                //skip error path
                    seen_with_update.emplace(path, true);
                else
                    seen_with_update.emplace(path, false);
            }
        }
    );
}

// updated data copied to index (mem: updated data*2)
void 
Exporter::update_index() {
    
    std::unordered_map<std::string, FileEvent>::iterator it = scan_result.begin();

    for (; it != scan_result.end(); ++it) { 
        std::unordered_map<std::string, bool>::iterator seen_it = seen_with_update.find(it->first);
        
        if (seen_it != seen_with_update.end()) {    //seen and not modified
            bool& is_modified = seen_it->second;
            if (!is_modified)   continue;
        }
        if (it->second.err.value() != 0)   continue;
        // not seen || seen & modified
        // new_entry.path = it->second.path;
        Entry new_entry;
        new_entry.node_type = it->second.node_type;
        new_entry.fp.modtime = it->second.modtime;
        new_entry.fp.size = it->second.size;

        index.update(std::move(it->second.path), std::move(new_entry));
    }


    //release memory of updated map
    seen_with_update = std::unordered_map<std::string, bool>{};
    // std::cerr << "Exporter::update_index: seen_with_update map released\n";
    // malloc_stats();
    AsyncLogger::logger().debug("Exporter::update_index: seen_with_update map released");
    AsyncLogger::logger().debug("Exporter::update_index; updated index - size: " + std::to_string(index.index_size()));
    std::unordered_map<std::string, Entry>::iterator deleted_it = deleted_map.begin();
    for (; deleted_it != deleted_map.end(); ++deleted_it) {
        index.erase(deleted_it->first);
    }
    AsyncLogger::logger().debug("Exporter::update_index; deleted index - size: " + std::to_string(index.index_size()));
}


void
Exporter::make_summary(ScanStats& summary, const JsonContent& c) const {

    switch (c.state) {
        case (EventState::Alive):
        {
            summary.alive_entries++;
            summary.total_entries++;

            switch (c.node_type) {
                case (NodeType::DIR):
                    summary.directory_count++;
                    break;
                case (NodeType::FILE):
                    summary.file_count++;
                    summary.total_file_size += static_cast<uint64_t>(c.size);
                    break;
                case (NodeType::LINK):
                    summary.symlink_count++;
                    break;
                default:
                    break;
            }
            break;
        }
        case (EventState::Deleted):
            summary.deleted_entries++;
            summary.total_entries++;
            break;
        case (EventState::Error):
            summary.error_entries++;
            summary.total_entries++;
            break;

        case (EventState::Canceled):
            summary.deleted_entries++;
            break;
        
        default:
            assert(false && "[Error] unknown entry state");         //do not define NDEBUG
    }
}


bool 
Exporter::export_summary(std::string& file_path) {

    if (!summary_collected)
        if (!export_result(file_path))
            return false;


    std::string record_time = formater::format_time(std::time(nullptr));
    file_path = out_dir + "/scan_summary_ID_" + std::to_string(scan_id) + "_T_" + record_time + ".json";
    std::string tmp_path = file_path + ".tmp";

    std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {       //fail to create file
        std::cerr << "Exporter cannot open indicated file";
        return false;
    }


    ofs << "{\n";
    ofs << "    \"summary_time\": \"" << record_time << "\",\n";
    ofs << "    \"states_summary\": {\n";
    ofs << "        \"total_entries\": "      << scan_summary.total_entries << ", \n";
    ofs << "        \"alive_entries\": "      << scan_summary.alive_entries << ", \n";
    if (is_canceled)
    ofs << "        \"not_scanned_entries\": "    << scan_summary.deleted_entries << ", \n";
    else
    ofs << "        \"deleted_entries\": "    << scan_summary.deleted_entries << ", \n";
    ofs << "        \"error_entries\": "      << scan_summary.error_entries << " \n";
    ofs << "    },\n";
    ofs << "    \"Content_summary\": {\n";
    ofs << "        \"directory_count\": "    << scan_summary.directory_count << ", \n";
    ofs << "        \"file_count\": "         << scan_summary.file_count << ", \n";
    ofs << "        \"total_file_bytes\": "    << scan_summary.total_file_size << ", \n";
    ofs << "        \"symlink_count\": "      << scan_summary.symlink_count << " \n";
    ofs << "    }\n";
    ofs << "}\n";

    //release mem of struct
    scan_summary = {};
    // std::cerr << "Exporter::export_summary: scan_summary struct released\n";
    // malloc_stats();
    AsyncLogger::logger().debug("Exporter::export_summary: scan_summary struct released");

    ofs.flush();
    if (!ofs.good()) {
        ofs.close();
        unlink(tmp_path.c_str());   //delete
        std::cerr << "Exporter::export_summary: flush error";
        return false;
    }

    ofs.close();

    if (rename(tmp_path.c_str(),file_path.c_str()) != 0) {
        unlink(tmp_path.c_str());   //delete
        std::cerr << "Exporter::export_summary: cannot replace path"; 
        return false;
    }
    std::cout << "successfully dump result to " << file_path << std::endl;

    summary_collected = false;
    return true;
}


//export result; collect summary
bool 
Exporter::export_result(std::string& file_path) {

    convert_to_map();
    AsyncLogger::logger().debug("Exporter::export_result - scan_collection vector data moved to map");


    //clear 
    scan_summary = ScanStats{};

    //identify state; update index
    check_deleted();
    update_index();
    

    std::string record_time = formater::format_time(std::time(nullptr));
    file_path = out_dir + "/scan_result_ID_" + std::to_string(scan_id) + "_T_" + record_time + ".json";
    //Temporary path used for crash-safe write
    //Only renamed to file_path after full write succeeds
    std::string tmp_path   = file_path + ".tmp";

    // Writing directly to file_path would risk leaving a corrupted file
    // std::ios::out  : open file for writing; create the file if it does not exist.
    // std::ios::trunc: truncate the file to zero length if it already exists.
    std::ofstream ofs(tmp_path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "Exporter cannot open indicated file";
        return false;
    }

    ofs << "{\n";
    // use \ to escape string
    ofs << "  \"result_time\": \"" << record_time << "\",\n";
    ofs << "  \"entries\": [\n";

    std::unordered_map<std::string, FileEvent>::iterator it = scan_result.begin();
    //last one without , at end
    // == first one without , at begin
    bool is_first = true;

    for (; it!=scan_result.end(); ++it) {

        FileEvent& e = it->second;
        EventState state;
        

        if (e.err.value() == 0) 
            state = EventState::Alive;
        else 
            state = EventState::Error;


        JsonContent c;
        formater::transform_event(std::move(e), c, state);

        std::string node_s = formater::format_node(c.node_type);
        std::string state_s = formater::format_state(c.state);

        if (node_s.empty() || state_s.empty()) {
            ofs.close();
            unlink(tmp_path.c_str());           // unlink() deletes the directory entry immediately
            std::cerr << "node out of index\n";
            return false;
        }


        //collect for summary
        make_summary(scan_summary, c);

        if (!is_first)
            ofs << ",\n";
        is_first = false;
            
        ofs << "    {\n";
        ofs << "      \"path\": \"" << c.path << "\",\n";
        ofs << "      \"type\": \"" << node_s << "\",\n";
        ofs << "      \"state\": \"" << state_s << "\",\n";
        ofs << "      \"err_msg\": \"" << c.err_msg << "\",\n";
        ofs << "      \"size\": " << c.size << ",\n";
        ofs << "      \"mtime\": \"" << c.time << "\" \n";
        ofs << "    }";

        ofs << "\n";
    }

    //release memory of result map
    scan_result = std::unordered_map<std::string, FileEvent>{};
    // std::cerr << "Exporter::export_result: scan_result map released\n";
    // malloc_stats();
    AsyncLogger::logger().debug("Exporter::export_result: scan_result map released");
    

    std::unordered_map<std::string, Entry>::iterator deleted_it = deleted_map.begin();
    for (; deleted_it!=deleted_map.end(); ++deleted_it) { 
        Entry& ent = deleted_it->second;

        EventState state = (is_canceled) ? EventState::Canceled : EventState::Deleted;

        JsonContent c;
        formater::transform_entry(std::move(ent), std::string(deleted_it->first), c, state);

        std::string node_s = formater::format_node(c.node_type);
        std::string state_s = formater::format_state(c.state);

        if (node_s.empty() || state_s.empty()) {
            ofs.close();
            unlink(tmp_path.c_str());           // unlink() deletes the directory entry immediately
            std::cerr << "node out of index\n";
            return false;
        }
        
        //collect for summary
        make_summary(scan_summary, c);
        
        if (!is_first)
        ofs << ",\n";
        is_first = false;
        ofs << "    {\n";
        ofs << "      \"path\": \"" << c.path << "\",\n";
        ofs << "      \"type\": \"" << node_s << "\",\n";
        ofs << "      \"state\": \"" << state_s << "\",\n";
        ofs << "      \"err_msg\": \"" << "" << "\",\n";
        ofs << "      \"size\": " << c.size << ",\n";
        ofs << "      \"mtime\": \"" << c.time << "\" \n";
        ofs << "    }";
        ofs << "\n";
    }

    //release memory of deleted map
    deleted_map = std::unordered_map<std::string, Entry>{};
    // std::cerr << "Exporter::export_result: deleted_map map released\n";
    // malloc_stats();
    AsyncLogger::logger().debug("Exporter::export_result: deleted_map map released");

    ofs << "  ]\n";
    ofs << "}\n";

    ofs.flush();
    if (!ofs.good()) {
        ofs.close();
        unlink(tmp_path.c_str());
        std::cerr << "flush error";
        return false;
    }

    ofs.close();

    // Atomically replace (or create) final result file
    // rename guarantees readers see either old file or new file, never a partial one
    if (rename(tmp_path.c_str(), file_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        std::cerr << "cannot replace path"; 
        return false;
    }

    std::cout << "successfully dump result to " << file_path << std::endl;

    summary_collected = true;
    return true;
}
