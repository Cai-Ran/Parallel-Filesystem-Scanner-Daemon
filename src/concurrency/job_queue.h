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
            AsyncLogger::logger().debug("JobQueue::QueueMetrics - submitted_failed not bound");
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

    //backpressure reject and response with full
    SubmitResult try_push(T&& item) {
        {
            std::lock_guard<std::mutex> lock(mtx);

            if (stop_flag) {
                if (metrics.submitted_failed) metrics.submitted_failed->fetch_add(1);
                return SubmitResult::Shutdown;
            }

            if (container.size() >= QUEUE_SIZE) { 
                if (metrics.submitted_failed) metrics.submitted_failed->fetch_add(1);
                return SubmitResult::Full;
            }

            container.push_back(std::move(item));

            if (metrics.submitted_total)    metrics.submitted_total ->fetch_add(1);
            if (metrics.queued_number)      metrics.queued_number   ->fetch_add(1);
        }

        cv.notify_one();

        return SubmitResult::Pushed;
    }


    void push(T&& item) {
        {
            std::unique_lock<std::mutex> lock(mtx);

            while (!stop_flag && container.size() >= QUEUE_SIZE)
                cv.wait(lock);

            if (stop_flag)  return;

            container.push_back(std::move(item));
            

            if (metrics.submitted_total)    metrics.submitted_total ->fetch_add(1);
            if (metrics.queued_number)      metrics.queued_number   ->fetch_add(1);
        }

        cv.notify_one();            //notify cv.wait in pop
    }


    bool pop(T& item) {
        {
            std::unique_lock<std::mutex> lock(mtx);

            while (container.empty() && !stop_flag)
                cv.wait(lock);

            if (container.empty() && stop_flag)
                return false;

            switch (policy) {
                case (QueueType::Fifo): {
                    item = std::move(container.front());
                    container.pop_front();
                    break;
                }
                case (QueueType::Lifo): {
                    item = std::move(container.back());
                    container.pop_back();
                    break;
                }
                default:
                    AsyncLogger::logger().error("JobQueue<T>::pop - unexpected policy; system error");
                    return false;
            }

            if (metrics.queued_number)  metrics.queued_number->fetch_sub(1);
        }

        cv.notify_one();        //notify cv.wait in push()

        return true;
    }


    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stop_flag = true;
        }
        cv.notify_all();
    }

    size_t jobs_in_queue() {
        std::lock_guard<std::mutex> lock(mtx);
        return container.size();
    }

};