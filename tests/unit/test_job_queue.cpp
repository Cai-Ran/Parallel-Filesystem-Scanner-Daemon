#include <macro.h>
#include <job_queue.h>
#include <thread>
#include <future>
#include <iostream>
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


// ======================================================================
// SINGLE THREAD:  basic function
// ======================================================================


// ======================
// test policy: 
// FIFO/LIFO behavior
// ======================
static bool test_policy_fifo () {
    const size_t que_size = 8;
    DummyMetrics m;
    JobQueue<int> fifo(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    for (int i=1; i<static_cast<int>(que_size) + 1; ++i) 
        EXPECT_EQ(fifo.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);

    int out = -1;
    for (int i=1; i<static_cast<int>(que_size) + 1; ++i) {
        EXPECT_TRUE(fifo.pop(out));
        EXPECT_EQ(out, i);
    }

    return true;
}

static bool test_policy_lifo () {
    const size_t que_size = 8;
    DummyMetrics m;
    JobQueue<int> lifo(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);
    
    for (int i=1; i<static_cast<int>(que_size) + 1; ++i)
        EXPECT_EQ(lifo.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);


    int out = -1;
    for (int i=static_cast<int>(que_size); i>0; --i) {
        EXPECT_TRUE(lifo.pop(out));
        EXPECT_EQ(out, i);
    }

    return true;
}

// ====================================
// test try_push(): 
// the behavior of fifo / lifo is same
// ====================================

static bool test_trypush_full() {
    const size_t que_size = 4;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);
    
    for (int i=1; i<static_cast<int>(que_size)+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);

    int over_attempt = 10;
    for (int i=1; i<over_attempt+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Full);

    EXPECT_EQ(m.submitted_total.load(), que_size);
    EXPECT_EQ(m.queued_number.load(), que_size);
    EXPECT_EQ(m.submitted_failed.load(), static_cast<uint64_t>(over_attempt));

    return true;
}

static bool test_trypush_after_shutdown() {
    const size_t que_size = 4;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);

    que.shutdown();

    int shutdown_attempt = static_cast<int>(que_size)+3;
    for (int i=1; i<shutdown_attempt+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Shutdown);

    EXPECT_EQ(m.submitted_total.load(), 0u);
    EXPECT_EQ(m.queued_number.load(), 0u);
    EXPECT_EQ(m.submitted_failed.load(), static_cast<uint64_t>(shutdown_attempt));

    return true;
}

static bool test_trypush_priority_shutdown_full() {
    const size_t que_size = 4;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);

    for (int i=1; i<static_cast<int>(que_size)+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);

    que.shutdown();

    int fail_attempt = 5;
    for (int i=1; i<fail_attempt+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Shutdown);

    EXPECT_EQ(m.submitted_total.load(), que_size);
    EXPECT_EQ(m.queued_number.load(), que_size);
    EXPECT_EQ(m.submitted_failed.load(), static_cast<uint64_t>(fail_attempt));

    return true;
}


// ========================================
// test pop(): 
// the behavior of fifo / lifo is different
// ========================================
static bool test_pop_empty_shutdown() {     //same behavior for lifo/fifo
    const size_t que_size = 4;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);

    que.shutdown();

    int fail_attempt = 3;
    int out = -1;
    for (int i=1; i<fail_attempt+1; ++i) 
        EXPECT_FALSE(que.pop(out));

    EXPECT_EQ(m.queued_number.load(), 0u);

    return true;
}

static bool test_pop_shutdown_drain_lifo() {
    const size_t que_size = 8;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Lifo, m.bind(), que_size);

    const int push_attempt = 5;
    for (int i=1; i<push_attempt+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);

    que.shutdown();

    const int pop_attempt = push_attempt;
    int out = -1;
    for (int i=pop_attempt; i>0; --i) {
        EXPECT_TRUE(que.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(que.pop(out));

    EXPECT_EQ(m.submitted_total.load(), static_cast<uint64_t>(push_attempt));
    EXPECT_EQ(m.queued_number.load(), 0u);
    EXPECT_EQ(m.submitted_failed.load(), 0u);

    return true;
}

static bool test_pop_shutdown_drain_fifo() {
    const size_t que_size = 8;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    int push_attempt = 5;
    for (int i=1; i<push_attempt+1; ++i) 
        EXPECT_EQ(que.try_push(std::move(i)), JobQueue<int>::SubmitResult::Pushed);

    que.shutdown();

    int pop_attempt = push_attempt;
    int out = -1;
    for (int i=1; i<pop_attempt+1; ++i) {
        EXPECT_TRUE(que.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(que.pop(out));

    EXPECT_EQ(m.submitted_total.load(), static_cast<uint64_t>(push_attempt));
    EXPECT_EQ(m.queued_number.load(), 0u);
    EXPECT_EQ(m.submitted_failed.load(), 0u);

    return true;
}

