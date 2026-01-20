#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

#include <async_logger.h>

template <typename T>


class JobQueue {

public:
    enum QueueType {
        Lifo,
        Fifo
    };

    struct QueueMetrics {
        std::atomic<uint64_t>* submitted_total;
        std::atomic<uint64_t>* submitted_failed;
        std::atomic<uint64_t>* queued_number;
    };

    enum SubmitResult {
      Pushed,
      Full,
      Shutdown  
    };

private:
    QueueType policy;
    QueueMetrics metrics;
    const size_t QUEUE_SIZE;

    std::deque<T> container;
    bool stop_flag = false;

    std::mutex mtx;
    std::condition_variable cv;

public:
    JobQueue(QueueType policy_, QueueMetrics metrics_, size_t size)
            : policy(policy_), metrics(metrics_), QUEUE_SIZE(size) 
    {
        if (!metrics.submitted_total) {
            AsyncLogger::logger().error("JobQueue::QueueMetrics - submitted_total not bound");
        }
        if (!metrics.submitted_failed) {
            AsyncLogger::logger().error("JobQueue::QueueMetrics - submitted_failed not bound");
        } 
        if (!metrics.queued_number) {
            AsyncLogger::logger().error("JobQueue::QueueMetrics - queued_number not bound");
        }
    };

    // copy forbidden; move forbidden; (due to mutex/cv)
    JobQueue(const JobQueue& q) = delete;               //JobQueue a = b; JobQueue a(b);
    JobQueue& operator=(const JobQueue& q) = delete;    // a = b;
    JobQueue(JobQueue&&) = delete;                      //JobQueue a(std::move(b));
    JobQueue& operator=(JobQueue&&) = delete;           // a = std::move(b);


