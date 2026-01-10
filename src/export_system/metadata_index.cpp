#include <metadata_index.h>
#include <async_logger.h>
#include <cassert>


void 
MetadataIndex::update(std::string&& path, Entry&& ent) {
    std::lock_guard<std::mutex> lock(mtx);
    std::unordered_map<std::string, Entry>::iterator it = index.find(path);
    if (it == index.end()) {
        index.emplace(std::move(path), std::move(ent));
        version_cnt++;
    }
    else if (it->second.fp != ent.fp) {
        it->second = std::move(ent);
        version_cnt++;
    }
}

bool 
MetadataIndex::get(const std::string& path, Entry& result) const {
    std::lock_guard<std::mutex> lock(mtx);
    std::unordered_map<std::string, Entry>::const_iterator it = index.find(path);       //const function

    if (it == index.end()) {
        return false;
    }

    result = it->second;
    return true;
}


bool
MetadataIndex::erase(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx);
    std::unordered_map<std::string, Entry>::const_iterator it = index.find(path);
    if (it == index.end()) {
        AsyncLogger::logger().error("path not in index, cannot erase");
        return false;
    }
    index.erase(it);
    version_cnt++;
    return true;
}


bool 
MetadataIndex::contains(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mtx);
    std::unordered_map<std::string, Entry>::const_iterator it = index.find(path);
    if (it == index.end()) return false;
    // if (it->second.state == Entry::EntryState::Alive) return true;
    return true;
};

size_t 
MetadataIndex::index_size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return index.size();
}

uint64_t
MetadataIndex::get_version_number() const {
    std::lock_guard<std::mutex> lock(mtx);
    return version_cnt;
}



