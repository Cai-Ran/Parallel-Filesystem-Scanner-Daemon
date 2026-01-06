#pragma once

#include <mutex>
#include <time.h>
#include <iostream>


class SyncLogger {
private:

    enum LogLevel {
        Info,
        Warn,
        Debug,
        Error
    };

    SyncLogger(){};
    
public:
    static SyncLogger& logger() {
        static SyncLogger logger;       //HERE create global logger
        return logger;
    }

private:
    // copy forbidden
    SyncLogger(const SyncLogger&) = delete;
    SyncLogger& operator=(const SyncLogger& other) = delete;
    // move forbidden
    SyncLogger(SyncLogger&&) = delete;
    SyncLogger& operator=(SyncLogger&&) = delete;

    std::mutex mtx;

    const char* logger_timestamp() {
        static char buf[32];            //avoid frequent heap allocation
        std::time_t t = std::time(nullptr);
        struct tm tm_buff{};
        localtime_r(&t, &tm_buff);
        std::strftime(buf, sizeof(buf), 
                    "%Y-%m-%d %H:%M:%S", &tm_buff);
        return buf;
    }

    void helper(LogLevel level, const std::string& msg) {
        static const std::string arr[4] = {"Info", "Warn", "Debug", "Error"};
        std::lock_guard<std::mutex> lock(mtx);  //protect cerr/buf
        std::cerr << logger_timestamp() << " [" << arr[level] << "] " << msg << "\n";
    }

    

public:
    void error(const char* msg) {helper(LogLevel::Error, msg);}
    void error(const std::string& msg) {error(msg.c_str());}
    void debug(const char* msg) {helper(LogLevel::Debug, msg);}
    void debug(const std::string& msg) {debug(msg.c_str());}
    void info(const char* msg) {helper(LogLevel::Info, msg);}
    void info(const std::string& msg) {info(msg.c_str());}
    void warn(const char* msg) {helper(LogLevel::Warn, msg);}
    void warn(const std::string& msg) {warn(msg.c_str());}


};