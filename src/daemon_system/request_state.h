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

