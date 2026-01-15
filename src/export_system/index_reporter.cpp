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



IndexStats
IndexReporter::make_index_summary() const {
    IndexStats stat;

    index.iterate_entry([&](const std::string& path, const Entry& ent)
        {
            (void)path;
            
            stat.take_time = std::time(nullptr);
            stat.entries_count++;

            switch (ent.node_type) {
                case (NodeType::DIR):
                    stat.dir_count++;
                    break;
                case (NodeType::FILE):
                    stat.file_count++;
                    stat.total_bytes += ent.fp.size;
                    break;
                case (NodeType::LINK):
                    stat.link_count++;
                    break;
                default:
                    break;
            }
        }
    );
    return stat;
}


bool
IndexReporter::export_index_summary(std::string& summary_path, time_t timestamp) const {

    IndexStats data = make_index_summary();

    std::string record_time = formater::format_time(timestamp);
    std::string v_number = std::to_string(version_number);

    summary_path = out_dir + "/index_summary_V_" + v_number + "_T_" + record_time + ".json";
    //Temporary path used for crash-safe write
    //Only renamed to final_path after full write succeeds
    std::string tmp_path   = summary_path + ".tmp";

    // Writing directly to final_path would risk leaving a corrupted file
    // std::ios::out  : open file for writing; create the file if it does not exist.
    // std::ios::trunc: truncate the file to zero length if it already exists.
    std::ofstream ofs(tmp_path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "IndexReporter cannot open indicated file";
        return false;
    }

    ofs << "{\n";
    ofs << "    \"index_version\": \""      << v_number << "\",\n";
    ofs << "    \"index_summary_time\": \"" << record_time << "\",\n";
    ofs << "    \"total_entries\": "        << data.entries_count << ", \n";
    ofs << "    \"directory_count\": "      << data.dir_count << ", \n";
    ofs << "    \"file_count\": "           << data.file_count << ", \n";
    ofs << "    \"total_file_bytes\": "     << data.total_bytes << " \n";
    ofs << "}\n";

    //release mem of struct
    data = {};
    AsyncLogger::logger().debug("IndexReporter::export_index_summary: IndexStats struct released");
    

    ofs.flush();
    if (!ofs.good()) {
        ofs.close();
        unlink(tmp_path.c_str());
        std::cerr << "flush error";
        return false;
    }

    ofs.close();

    if (rename(tmp_path.c_str(), summary_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        std::cerr << "cannot replace path"; 
        return false;
    }
    std::cout << "successfully dump result to " << summary_path << std::endl;
    return true;

}
