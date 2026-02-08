#pragma once
#include <cstdint>

enum class RequestState : uint8_t {
    PENDING,
    DROPPED,

    RUNNING, 
    CANCELED,
    DONE,
    FAILED,

    DISPATCHING,            // internal for scheduler only: between PENDING and RUNNING 
                            // Scheduler::run() holds lock gap
};


// RUNNING      -> DONE
// RUNNING      -> CANCELED
// PENDING      -> DROPPED
// PENDING      -> DISPATCHING  (run() dequeues, calls manager outside lock)
// DISPATCHING  -> RUNNING      (manager accepted)
// DISPATCHING  -> CANCELED     (user canceled during dispatch)
// DISPATCHING  -> FAILED       (manager internal error)

