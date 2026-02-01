#pragma once

#include <iostream>


// static to only visible in this file; C-style 
static bool report_fail(const char* condition, const char* file_name, int line) {
    std::cerr << "[TEST FAIL]" << file_name << ":" << line << " -" << condition << "\n"; 
    return false;
}

// do {} while (0)  to pack the macro block
#define EXPECT_TRUE(condition) \
    do { \
        if (!(condition)) \
        return report_fail(#condition, __FILE__, __LINE__); \
    } while (0);

#define EXPECT_FALSE(condition) \
    do { \
        if ((condition)) \
        return report_fail(#condition, __FILE__, __LINE__); \
    } while (0);

#define EXPECT_EQ(res1, res2) \
    do { \
        if (!((res1) == (res2))) \
        return report_fail(#res1 " == " #res2, __FILE__, __LINE__); \
    } while (0);
