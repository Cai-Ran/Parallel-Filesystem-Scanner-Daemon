#pragma once

#include <metadata_index.h>
#include <formater.h>
#include <stats.h>




class IndexReporter {

private:
    const MetadataIndex& index;
    const uint64_t version_number;
    std::string out_dir;

    IndexStats make_index_summary() const;



public:
    IndexReporter(const MetadataIndex& index, const uint64_t version_number);

    bool set_export_dir(const std::string& dir);

    bool export_index_detail(std::string& detail_path, time_t timestamp);
    bool export_index_summary(std::string& summary_path, time_t timestamp) const;

};
