#include <scheduler.h>
#include <manager.h>
#include <async_logger.h>

#include <climits>
#include <cstring>
#include <cerrno>



SubmitScanResult
Scheduler::submit_scan_root(std::string&& path, uint64_t& scan_id) {
    scan_id = 0;    //invalid id; to avoid missed initialization
    std::string root_path = normalize_path(path);
    if (root_path.empty()) {
        AsyncLogger::logger().error("Scheduler::submit_scan_root - root path invalid; skip scan_id creation");
        return SubmitScanResult::Invalid;
    }

    uint64_t new_id = gen_scan_id();
    
    {
        std::lock_guard<std::mutex> lock(mtx);

        if (stop_flag)                                           return SubmitScanResult::Shutdown;
        if (pending_scans >= static_cast<int>(QUEUE_MAX_SIZE))   return SubmitScanResult::QueueFull;
        //notify user queue is full wait later

        std::pair<std::unordered_map<uint64_t, RequestState>::iterator, bool> res = \
            state_map.emplace(new_id, RequestState::PENDING);
        if (!res.second) {
            AsyncLogger::logger().error("scan id already recorded in state map");
            return SubmitScanResult::InternalError;
        }

        PendingRoot data(new_id, std::move(root_path));
        pending_queue.push_back(data);

        Metrics::measurement().scan_pending.fetch_add(1);
        pending_scans++;
    }
    
    cv.notify_one();

    scan_id = new_id;
    return SubmitScanResult::Ok;
}



// run() is executed on a dedicated thread to dispatch scans 
void
Scheduler::run() {

    PendingRoot data;

    while (1) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            while (!stop_flag && 
                    (pending_queue.empty() || pending_scans == 0
                    || manager_queue_full
                    || running_scans >= static_cast<int>(MAX_CONCURRENT_SCAN))
            ) 
            {
                cv.wait(lock);
            }
                

            if (stop_flag) break;

            data = pending_queue.front();   
                
            std::unordered_map<uint64_t, RequestState>::iterator it = state_map.find(data.scan_id);

            if (it == state_map.end() || (it != state_map.end() && it->second != RequestState::PENDING)) {
                if (it == state_map.end())  AsyncLogger::logger().error("Scheduler::run() - scan id not in state map");

                if (!pending_queue.empty() && pending_queue.front().scan_id == data.scan_id) pending_queue.pop_front();      
                if (Metrics::measurement().scan_pending.load()) Metrics::measurement().scan_pending.fetch_sub(1); 
                if (it != state_map.end() && it->second != RequestState::DROPPED) pending_scans--;
                /*
                DROPPED:    PENIDNG canceled                        canceled dealed with pending_scans
                CANCELED:   DISPATCHING canceled                    before if (manager_accept) {pop;}
                DONE:       DISPATCHING notify_scan_finished        before if (manager_accept) {pop;}
                */
            
                continue;
            }
            else it->second = RequestState::DISPATCHING;        //PENDING->DISPATCHING
        }

        bool manager_accept = false;
        bool queue_full = false;

        try { 
            // 1. manager_accept                 -> RUNNING
            // 2. !manager_accept && queue_full -> PENDING
            // 3. !manager_accept && !queue_full -> CANCELD (shutdown / internal error)
            manager_accept = manager.start_new_scan(data.scan_id, data.root, queue_full);
        } catch (...) {
            manager_accept = false;     //internal error
        }
        /*
            we need to deal with the problem:
            1. manager accept:                      DISPATCHING -> RUNNING
            2. manager reject:                      DISPATCHING -> PENDING
            3. user canceled during DISPATCHING :   DISPATCHING -> CANCELED
                    cancel mark CANCELED in advanced; no dropped
        */

        bool cancel_after_manager_accept = false;

        {
            std::lock_guard<std::mutex> lock(mtx);
            std::unordered_map<uint64_t, RequestState>::iterator it = state_map.find(data.scan_id);
            if (it == state_map.end()) {
                AsyncLogger::logger().error("Scheduler::run() - scan id not in state map");
                continue;
            }
            
            if (it->second == RequestState::CANCELED) {     //DISPATCHING -> CANCELED
                if (!pending_queue.empty() && pending_queue.front().scan_id == data.scan_id) pending_queue.pop_front();      
                if (Metrics::measurement().scan_pending.load()) Metrics::measurement().scan_pending.fetch_sub(1); 
                pending_scans--;

                if (manager_accept) {
                    cancel_after_manager_accept = true;
                    running_scans++;
                    Metrics::measurement().scan_running.fetch_add(1);       
                }
            }
            else if (it->second == RequestState::DISPATCHING) {
                if (manager_accept) {                       //DISPATCHING -> RUNNING
                    running_scans++;
                    Metrics::measurement().scan_running.fetch_add(1);       
                    it->second = RequestState::RUNNING;

                    if (!pending_queue.empty() && pending_queue.front().scan_id == data.scan_id) pending_queue.pop_front();      
                    if (Metrics::measurement().scan_pending.load()) Metrics::measurement().scan_pending.fetch_sub(1); 
                    pending_scans--;
                }
                else {
                    if (queue_full) {                       //DISPATCHING -> PENDING
                        manager_queue_full = true;          //set flag for while() condition
                        it->second = RequestState::PENDING;
                    }
                    else {                                  //DISPATCHING -> FAILED (theoretically not happen)
                        if (!pending_queue.empty() && pending_queue.front().scan_id == data.scan_id) pending_queue.pop_front();      
                        if (Metrics::measurement().scan_pending.load()) Metrics::measurement().scan_pending.fetch_sub(1); 
                        pending_scans--;
                        it->second = RequestState::FAILED;
                    }
                }
            }
        }

        if (cancel_after_manager_accept)
            manager.cancel_scan(data.scan_id);
    }
}

