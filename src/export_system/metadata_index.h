#pragma once

#include <unordered_map>
#include <string>
#include <mutex>
// #include <shared_mutex>

#include <file_event.h>
#include <entry.h>


class MetadataIndex {
private:
    std::unordered_map<std::string, Entry> index;
    uint64_t version_cnt = 0;
    mutable std::mutex mtx;                 //mutable for const member function

public: 
    //template + lambda : template must implement in header; caller implement in cpp
    template<typename Fn>
    void iterate_entry(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mtx);
        std::unordered_map<std::string, Entry>::const_iterator it = index.begin();
        for (; it!=index.end(); ++it)
            fn(it->first, it->second);
    };

    // void apply(const FileEvent& event);
    void update(std::string&& path, Entry&& ent);
    bool erase(const std::string& path);

    bool get(const std::string& path, Entry& result) const;
    size_t index_size() const;
    bool contains(const std::string& path) const;    //find Active entry
    uint64_t get_version_number() const;

};
