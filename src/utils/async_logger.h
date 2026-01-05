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

        size_t MAX_QUEUE_SIZE;
        
        std::mutex mtx;
        std::condition_variable cv;
        // protect
        std::queue<LogItem> log_queue;
        bool stop = false;

        std::thread log_worker;
        std::string log_path;

        std::mutex cerr_mtx;


    public:
        AsyncLogger()
            :MAX_QUEUE_SIZE(!Config::cfg().asynclogger().queue_max_size ? 1
                            :Config::cfg().asynclogger().queue_max_size) 
        {
            Metrics::measurement().const_logger_pending_queue_size = MAX_QUEUE_SIZE;
        };  
        
        
        // switch between global constructor | default constructor 
        // must be static to be call without object constructed      
        static void switch_logger(AsyncLogger& logger) {
            logger_ptr = &logger;
        }
        static AsyncLogger& logger() {
            static AsyncLogger global_logger;
            if (!logger_ptr) logger_ptr = &global_logger;
            return *logger_ptr;
        }
        ~AsyncLogger() { shutdown();}
        

    private:
        static AsyncLogger* logger_ptr;
        //copy forbidden
        AsyncLogger(const AsyncLogger&) = delete;
        AsyncLogger& operator=(AsyncLogger&) = delete;
        //move forbidden
        AsyncLogger(AsyncLogger&&) = delete;
        AsyncLogger& operator=(AsyncLogger&&) = delete;

        void helper(LogLevel level, const std::string& msg);
        void print_to_terminal(LogItem item);

    //API
    public:
        bool start();
        bool consume(LogItem& job);
        void error(const std::string& msg)  { helper(Error, msg); }
