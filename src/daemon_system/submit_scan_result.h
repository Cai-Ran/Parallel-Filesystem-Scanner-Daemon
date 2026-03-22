#pragma once
// Scheduler Pending Queue 
enum class SubmitScanResult {
    Ok,
    Invalid,
    QueueFull,
    Shutdown,
    InternalError,
    OverlapConflict
};

