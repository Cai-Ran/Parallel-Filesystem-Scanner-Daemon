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

# -------------------------
# Normal compile
# -------------------------
# JOB_QUEUE
add_executable(test_job_queue ${JOB_QUEUE_TEST_SOURCES})
# PRIVATE: condition only for this target
target_include_directories(test_job_queue PRIVATE ${COMMON_INCLUDE_DIRS})
target_compile_options(test_job_queue PRIVATE ${COMMON_COMPILE_OPTIONS})
# enable POSIX threads
target_link_options(test_job_queue PRIVATE -pthread)
# link POSIX thread library
target_link_libraries(test_job_queue PRIVATE pthread)

# THREAD_POOL
add_executable(test_thread_pool ${THREAD_POOL_TEST_SOURCES})
target_include_directories(test_thread_pool PRIVATE ${COMMON_INCLUDE_DIRS})
target_compile_options(test_thread_pool PRIVATE ${COMMON_COMPILE_OPTIONS})
target_link_options(test_thread_pool PRIVATE -pthread)
target_link_libraries(test_thread_pool PRIVATE pthread)

# -------------------------
# ASAN compile
# -------------------------
# JOB_QUEUE
add_executable(test_job_queue_asan ${JOB_QUEUE_TEST_SOURCES})
target_include_directories(test_job_queue_asan PRIVATE ${COMMON_INCLUDE_DIRS})

# -fsanitize=address: AddressSanitizer: check memory bugs
# -fno-omit-frame-pointer do not skip frame pointer

target_compile_options(test_job_queue_asan PRIVATE
    ${COMMON_COMPILE_OPTIONS}
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_options(test_job_queue_asan PRIVATE
    -pthread
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_libraries(test_job_queue_asan PRIVATE pthread)

# THREAD_POOL
add_executable(test_thread_pool_asan ${THREAD_POOL_TEST_SOURCES})
target_include_directories(test_thread_pool_asan PRIVATE ${COMMON_INCLUDE_DIRS})
target_compile_options(test_thread_pool_asan PRIVATE
    ${COMMON_COMPILE_OPTIONS}
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_options(test_thread_pool_asan PRIVATE
    -pthread
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_libraries(test_thread_pool_asan PRIVATE pthread)

# -------------------------
# TSAN compile
# -------------------------
# JOB_QUEUE
add_executable(test_job_queue_tsan ${JOB_QUEUE_TEST_SOURCES})
target_include_directories(test_job_queue_tsan PRIVATE ${COMMON_INCLUDE_DIRS})

# -fsanitize=thread :  ThreadSanitizer : detect data race

target_compile_options(test_job_queue_tsan PRIVATE
    ${COMMON_COMPILE_OPTIONS}
    -fsanitize=thread
    -fno-omit-frame-pointer
)
target_link_options(test_job_queue_tsan PRIVATE
    -pthread
    -fsanitize=thread
    -fno-omit-frame-pointer
)
target_link_libraries(test_job_queue_tsan PRIVATE pthread)

# THREAD_POOL
add_executable(test_thread_pool_tsan ${THREAD_POOL_TEST_SOURCES})
target_include_directories(test_thread_pool_tsan PRIVATE ${COMMON_INCLUDE_DIRS})
target_compile_options(test_thread_pool_tsan PRIVATE
    ${COMMON_COMPILE_OPTIONS}
    -fsanitize=thread
    -fno-omit-frame-pointer
)
target_link_options(test_thread_pool_tsan PRIVATE
    -pthread
    -fsanitize=thread
    -fno-omit-frame-pointer
)
target_link_libraries(test_thread_pool_tsan PRIVATE pthread)
