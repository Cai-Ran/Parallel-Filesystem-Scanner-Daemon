cmake_minimum_required(VERSION 3.20)
project(TEST CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# sources inluded in utils (asyncLogger/formater/config)
set(UTILS_SUPPORT_SOURCES
    ${CMAKE_SOURCE_DIR}/../src/utils/async_logger.cpp
    ${CMAKE_SOURCE_DIR}/../src/utils/config.cpp
    ${CMAKE_SOURCE_DIR}/../src/utils/formater.cpp
)

# sources inluded in job_queue_test 
set(JOB_QUEUE_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/unit/test_job_queue.cpp
    ${UTILS_SUPPORT_SOURCES}
)

# source included in thread_pool_test 
set(THREAD_POOL_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/unit/test_thread_pool.cpp
    ${UTILS_SUPPORT_SOURCES}
)

set(COMMON_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/../src/common/
    ${CMAKE_SOURCE_DIR}/../src/concurrency
    ${CMAKE_SOURCE_DIR}/../src/utils
    ${CMAKE_SOURCE_DIR}/../src/export_system
)

# all warning / extra warning / debug symbols  
set(COMMON_COMPILE_OPTIONS
    -Wall
    -Wextra
    -g
)

