#pragma once

#include <entry.h>
#include <string>


struct JsonContent {
    std::string path;
    NodeType node_type;
    EventState state;
    std::string err_msg;
    uint64_t size;
    std::string time;
};


namespace formater {

    bool validate_outdir(const std::string& path);
    bool validate_outpath(const std::string& path);

    void transform_entry(Entry&& e, std::string&& path, JsonContent& json, EventState state);
    void transform_event(FileEvent&& e, JsonContent& json, EventState state);

    std::string format_node(NodeType node_type);
    std::string format_state(EventState state);

    std::string format_time(const time_t t);

    bool paths_overlap(const std::string& a, const std::string& b);

};

