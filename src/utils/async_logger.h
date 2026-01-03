#pragma once
#include <formater.h>
#include <config.h>
#include <metrics.h>

#include <queue>
#include <string>
#include <ctime>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iostream>



// Async logger usually uses a background thread, but the key point is this:
// the thread calling log() does NOT perform I/O. It only enqueues the message
// and returns immediately; a background worker drains the queue and writes.
//
// Sync logger: log() writes to cerr/file directly -> can block.
// Async logger: log() enqueues -> returns fast; worker thread writes later.
// This reduces blocking on worker threads and improves concurrency.
// (multiple producer) thread ask write log + a worker thread write for IO (one consumer)


class AsyncLogger {
    private:

        enum LogLevel {
            Info,
            Warn,
            Debug,
            Error
        };

        struct LogItem {
            LogLevel level;
            std::string msg;
            time_t timestamp;
            std::thread::id tid;    //for debug
        };
