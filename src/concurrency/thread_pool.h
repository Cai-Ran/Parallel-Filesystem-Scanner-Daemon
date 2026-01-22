#pragma once

#include <job_queue.h>

#include <functional>
#include <thread>
#include <vector>

template <typename T>

class ThreadPool {

using JobFn = std::function<void(T)>;


private:
    JobQueue<T> job_queue;
    const size_t NUM_THREADS;
    JobFn job_fn;
    std::vector<std::thread> pool;

private:
    void work_forever() {
        T job;
        while (job_queue.pop(job)) {
            job_fn(std::move(job));
        }
        job_queue.shutdown();
    }

public:
    ThreadPool(typename JobQueue<T>::QueueType policy, 
            typename JobQueue<T>::QueueMetrics metrics,
            size_t queue_size,
            size_t num_threads)
        :job_queue(policy, std::move(metrics), queue_size), NUM_THREADS(num_threads) {};

    // copy forbbidden; move forbidden; (due to mutex/cv)
    ThreadPool(const ThreadPool& q) = delete;               //ThreadPool a = b; ThreadPool a(b);
    ThreadPool& operator=(const ThreadPool& q) = delete;    // a = b;
    ThreadPool(ThreadPool&&) = delete;                      //ThreadPool a(std::move(b));
    ThreadPool& operator=(ThreadPool&&) = delete;           // a = std::move(b);




    bool start(JobFn worker_function) {
        job_fn = std::move(worker_function);

        try {
            pool.reserve(NUM_THREADS);

            for (size_t i=0; i<NUM_THREADS; ++i) 
                pool.emplace_back([this]{work_forever();});

        } catch (...) {
            AsyncLogger::logger().error("ThreadPool::start() failed");
            shutdown();
            return false;
        }
        return true;
    }


    void shutdown() {
        job_queue.shutdown();
        
        for (size_t i=0; i<pool.size(); ++i) {
            if (pool[i].joinable())
                pool[i].join();
        }

        pool.clear();
        job_fn = {};
    }

    typename JobQueue<T>::SubmitResult submit(T&& data) {
        return job_queue.try_push(std::move(data));
    }

    size_t jobs_in_queue() {
        return job_queue.jobs_in_queue();
    }

};