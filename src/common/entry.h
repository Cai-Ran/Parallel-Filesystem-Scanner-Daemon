#pragma once

#include <cstdint>
#include <string>
#include <file_event.h>
// Data structures stored inside index map.
// Must be default-constructible and initialized to a safe state
// because unordered_map[] may create entries implicitly.

struct FingerPrint {
    time_t modtime = 0;
    uint64_t size = 0;

    //overload the comparison operators

    bool match(const FileEvent& event) const {
        return (modtime == event.modtime && size == event.size);
    }

    bool operator==(const FingerPrint& another) const {
        return (modtime == another.modtime && size == another.size);
    }

    bool operator!=(const FingerPrint& another) const {
        return !(*this == another);                     
        // return (*this != another);                  //NOTE: [this] call FingerPrint::operator!=  infinitely call itself
    }
};

struct Entry {

    // std::string path = "";      //duplicated and useful but drained too much mem
    NodeType node_type = NodeType::UNKNOWN;
    FingerPrint fp;                     // used to detect metadata changes
    // std::string hash;                //optional: content hash
};

