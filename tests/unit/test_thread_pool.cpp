#include <macro.h>
#include <thread_pool.h>
#include <thread>
#include <functional>
#include <atomic>
#include <chrono>

// ===========================
// to decouple from Metrics, 
// create psuedo metrics
// ===========================
struct DummyMetrics {
    std::atomic<uint64_t> submitted_total{0};
    std::atomic<uint64_t> submitted_failed{0};
    std::atomic<uint64_t> queued_number{0};

    JobQueue<int>::QueueMetrics bind() {
        JobQueue<int>::QueueMetrics metrics {};
        metrics.submitted_total = &submitted_total;
        metrics.submitted_failed = &submitted_failed;
        metrics.queued_number = &queued_number;
        return metrics;
    }
};


// ======================================================
// SINGLE THREAD test :  
// verify high-level multi-thread behavior of thead pool
// ======================================================

static bool test_start_and_work_forever_till_shutdown() {
    const size_t que_size = 16;
    const size_t num_threads = 4;
    DummyMetrics m;
    ThreadPool<int> pool(JobQueue<int>::QueueType::Fifo, m.bind(), que_size, num_threads);

    std::atomic<uint64_t> push_sum{0};
    std::atomic<uint64_t> pop_sum{0};

    std::function<void(int)> job_fn = \
        [&pop_sum](int x) {
            pop_sum.fetch_add(x);
        };

    bool started = pool.start(job_fn);
    EXPECT_TRUE(started);
    
    for (int i=1; i<static_cast<int>(que_size); ++i) {
        int data = i;
        JobQueue<int>::SubmitResult result = pool.submit(std::move(data));
        EXPECT_EQ(result, JobQueue<int>::SubmitResult::Pushed);
        push_sum.fetch_add(data);
    }

    pool.shutdown();

    EXPECT_EQ(push_sum.load(), pop_sum.load());

    return true;
}


static bool test_submit_after_shutdown() {
    const size_t que_size = 16;
    const size_t num_threads = 4;
    DummyMetrics m;
    ThreadPool<int> pool(JobQueue<int>::QueueType::Fifo, m.bind(), que_size, num_threads);

    std::atomic<uint64_t> push_sum{0};
    std::atomic<uint64_t> pop_sum{0};

    std::function<void(int)> job_fn = \
        [&pop_sum](int x) {
            pop_sum.fetch_add(x);
        };

    
    bool started = pool.start(job_fn);
    EXPECT_TRUE(started);

    pool.shutdown();

    for (int i=1; i<static_cast<int>(que_size); ++i) {
        int data = i;
        JobQueue<int>::SubmitResult result = pool.submit(std::move(data));
        EXPECT_EQ(result, JobQueue<int>::SubmitResult::Shutdown);

        if (result == JobQueue<int>::SubmitResult::Pushed)
            push_sum.fetch_add(data);
    }   

    EXPECT_EQ(pop_sum.load(), 0u);
    EXPECT_EQ(push_sum.load(), 0u);

    return true;
}


static bool test_shutdown_drain() {
    const size_t que_size = 256;
    const size_t num_threads = 2;
    DummyMetrics m;
    ThreadPool<int> pool(JobQueue<int>::QueueType::Fifo, m.bind(), que_size, num_threads);

    std::atomic<uint64_t> push_sum{0};
    std::atomic<uint64_t> pop_sum{0};

    std::function<void(int)> job_fn = \
        [&pop_sum](int x) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            pop_sum.fetch_add(x);

        };

    bool started = pool.start(job_fn);
    EXPECT_TRUE(started);
    
    const int push_num = 128;
    for (int i=1; i<push_num; ++i) {
        int data = i;
        JobQueue<int>::SubmitResult result = pool.submit(std::move(data));
        EXPECT_EQ(result, JobQueue<int>::SubmitResult::Pushed);
        push_sum.fetch_add(data);
    }

    pool.shutdown();

    EXPECT_EQ(push_sum.load(), pop_sum.load());

    return true;
}


static bool test_shutdown_wake() {
    const size_t que_size = 16;
    const size_t num_threads = 4;
    DummyMetrics m;
    ThreadPool<int> pool(JobQueue<int>::QueueType::Fifo, m.bind(), que_size, num_threads);

    std::atomic<uint64_t> pop_sum{0};

    std::function<void(int)> job_fn = \
        [&pop_sum](int x) {
            pop_sum.fetch_add(x);
        };

    
    bool started = pool.start(job_fn);
    EXPECT_TRUE(started);

    pool.shutdown();
    // If shutdown deadlocks while workers are blocked on pop(), test never reaches the assertion below.
    EXPECT_EQ(pop_sum.load(), 0u);

    return true;
}

// ==============
// START TEST
// ==============

int main() {
    int count_passed = 0;
    int count_failed = 0;
    int count_total = 0;

    static std::string result[2] = {"FAIL", "PASS"};

    std::function<void(bool (*)(), const char*)> run_test = \
        [&](bool (*fn)(), const char* name) 
        {
            count_total++;
            bool passed = fn();
            if (passed) count_passed++;
            else        count_failed++;

            std::cerr << "[" << result[passed] << "] " << name << "\n";
        };

    run_test(test_start_and_work_forever_till_shutdown, "test_start_and_work_forever_till_shutdown");
    run_test(test_submit_after_shutdown, "test_submit_after_shutdown");
    run_test(test_shutdown_drain, "test_shutdown_drain");
    run_test(test_shutdown_wake, "test_shutdown_wake");

    std::cerr << "-----------------------------\n";
    std::cerr << "TOTAL : " << count_total << "\n";
    std::cerr << "PASS  : " << count_passed << "\n";
    std::cerr << "FAIL  : " << count_failed << "\n";

    return (count_failed == 0) ? 0 : 1;
}
