#include <exporter.h>
#include <async_logger.h>

#include <unistd.h>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <cassert>
#include <vector>


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
                if (!ent.fp.match(it->second))  //in index also in this scan, check if modified
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
    Entry new_entry;
    std::unordered_map<std::string, FileEvent>::iterator it = scan_result.begin();

    for (; it != scan_result.end(); ++it) { 
        std::unordered_map<std::string, bool>::iterator seen_it = seen_with_update.find(it->first);

        if (seen_it != seen_with_update.end()) {    //seen and not modified
            bool& is_modified = seen_it->second;
            if (!is_modified)   continue;
        }
        // not seen || seen & modified
        // new_entry.path = it->second.path;
        new_entry.node_type = it->second.node_type;
        new_entry.fp.modtime = it->second.modtime;
        new_entry.fp.size = it->second.size;

        index.update(std::move(it->second.path), std::move(new_entry));
    }


    //release memory of updated map
    seen_with_update = std::unordered_map<std::string, bool>{};
    AsyncLogger::logger().debug("Exporter::update_index: seen_with_update map released");
    AsyncLogger::logger().debug("Exporter::update_index; updated index - size: " + std::to_string(index.index_size()));
    std::unordered_map<std::string, Entry>::iterator deleted_it = deleted_map.begin();
    for (; deleted_it != deleted_map.end(); ++deleted_it) {
        index.erase(deleted_it->first);
    }
    AsyncLogger::logger().debug("Exporter::update_index; deleted index - size: " + std::to_string(index.index_size()));
}

