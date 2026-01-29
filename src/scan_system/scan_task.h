#pragma once
#include <file_event.h>
#include <async_logger.h>

#include <string>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>


// modified data structure from map to vector
// duplicate removal is operated by single thread exporter


class ScanResult {
private:
    std::mutex mtx;
    std::vector<FileEvent> collected_results;
    std::atomic<bool> frozen{false};

public:
/*
    Member functions do not increase object size.
*/
    //Scanner API
    //usage (std::move(event))
    void record(FileEvent&& event) {

        std::lock_guard<std::mutex> lock(mtx);

        if (frozen.load()) {
            AsyncLogger::logger().error("ScanResult record while frozen");
            return;
        }

        collected_results.push_back(std::move(event));
    }

    //Mannager API
    //check unfinished job outside
    std::vector<FileEvent>
    freeze_move() {

        std::lock_guard<std::mutex> lock(mtx);

        frozen.store(true);
        return std::move(collected_results);
    }


};

