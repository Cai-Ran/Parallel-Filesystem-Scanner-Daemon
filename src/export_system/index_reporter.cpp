#include <index_reporter.h>
#include <async_logger.h>

#include <fstream>
#include <unistd.h>



IndexReporter::IndexReporter(const MetadataIndex& index_, const uint64_t version_number_)
    :index(index_), version_number(version_number_) {}


bool
IndexReporter::set_export_dir(const std::string& path) {
    if (!formater::validate_outdir(path))
        return false;

    out_dir = path;
    return true;
}
