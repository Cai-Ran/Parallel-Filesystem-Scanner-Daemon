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


// ======================================================================
// MULTI THREAD:  basic function
// ======================================================================
static std::pair<bool, int> worker(JobQueue<int>* que, std::promise<void> promise_thread_start) {
    promise_thread_start.set_value();
    int out = -1;
    bool success = que->pop(out);
    std::pair<bool, int> res = {success, out};
    return res;
}

static bool test_pop_wait_push() {
    const size_t que_size = 4;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    //async:    create a thread: wait for pop
    //promise:  mark thread start time
    std::promise<void> promise_thread_start;
    std::future<void> future_start_result = promise_thread_start.get_future();

    std::future<std::pair<bool, int>> future_thread_result = \
                                    std::async(std::launch::async, worker, &que, std::move(promise_thread_start));     
        //que不能複製必須傳pointer//promise不能複製必須move

    bool thread_ready = ((future_start_result.wait_for(std::chrono::milliseconds(100))) == std::future_status::ready);
    EXPECT_TRUE(thread_ready);              //wait 100ms for thread to start (typically 100)

    bool thread_wait_pop = ((future_thread_result.wait_for(std::chrono::milliseconds(300))) == std::future_status::timeout); 
    EXPECT_TRUE(thread_wait_pop);           //pop blocked, so wait after 5000 still not ready -> timeout

    int data = 40;
    EXPECT_EQ(que.try_push(std::move(data)), JobQueue<int>::SubmitResult::Pushed);
    bool thread_get_pop = ((future_thread_result.wait_for(std::chrono::milliseconds(50))) == std::future_status::ready); 
    EXPECT_TRUE(thread_get_pop);            //queue not empty. so pop only wait for 50ms get data -> ready

    std::pair<bool, int> result = future_thread_result.get();
    EXPECT_TRUE(result.first);
    EXPECT_EQ(result.second, data);

    return true;
}

static bool test_pop_shutdown() {
    const size_t que_size = 2;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    std::promise<void> promise_thread_start;
    std::future<void> future_start_result = promise_thread_start.get_future();

    std::future<std::pair<bool, int>> future_thread_result = \
                                std::async(std::launch::async, worker, &que, std::move(promise_thread_start));

    bool thread_ready = ((future_start_result.wait_for(std::chrono::milliseconds(100))) == std::future_status::ready);
    EXPECT_TRUE(thread_ready);

    bool thread_wait_pop = ((future_thread_result.wait_for(std::chrono::milliseconds(300))) == std::future_status::timeout);
    EXPECT_TRUE(thread_wait_pop);

    que.shutdown();
    bool thread_wake = ((future_thread_result.wait_for(std::chrono::milliseconds(50))) == std::future_status::ready);
    EXPECT_TRUE(thread_wake);

    std::pair<bool, int> result = future_thread_result.get();
    EXPECT_FALSE(result.first);
    
    return true;
}

static std::pair<bool, int> worker_no_promise(JobQueue<int>* que) {
    int out = -1;
    bool success = que->pop(out);
    std::pair<bool, int> res = {success, out};
    return res;
}

// if queue empty -> threads wait -> shutdown notify all
static bool test_pop_wake_all_waiting_by_shutdown() {

    const size_t que_size = 2;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    using future_type = std::future<std::pair<bool, int>>;    
    std::vector<future_type> future_threads_result;

    int consumer_count = 4;
    future_threads_result.reserve(consumer_count);

    for (int i=0; i<consumer_count; ++i) {
        future_threads_result.push_back(
            std::async(std::launch::async, worker_no_promise, &que)
        );
    }
    // threads waiting due to que empty
    for (int i=0; i<consumer_count; ++i) {
        bool thread_wait_pop = (future_threads_result[i].wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
        EXPECT_TRUE(thread_wait_pop);
    }

    que.shutdown();
    // all threads wake
    for (int i=0; i<consumer_count; ++i) {
        bool thread_wake = (future_threads_result[i].wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
        EXPECT_TRUE(thread_wake);
        std::pair<bool,int> result = future_threads_result[i].get();
        EXPECT_FALSE(result.first);
    }

    return true;
}

//stress test: multi producer + multi consumer
static bool test_concurrency_stress() {

    const size_t que_size = 128;
    DummyMetrics m;
    JobQueue<int> que(JobQueue<int>::QueueType::Fifo, m.bind(), que_size);

    const int num_producer = 5;
    const int num_consumer = 5;
    const int data_per_producer = 200;
    // total push > que_size to test que full
    const int total_data = num_producer * data_per_producer;

    std::atomic<uint64_t> produced_success{0};
    std::atomic<uint64_t> consumed_success{0};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(num_producer);
    consumers.reserve(num_consumer);

    //STEP1: first create consumer; consumer wait
    for (int i=0; i<num_consumer; ++i) {
        //consumer keep pop until empty & stop
        consumers.emplace_back(
            [&que, &consumed_success]() 
            {
                while (true) {
                    int out = -1;
                    bool success = que.pop(out);
                    if (!success)   break;          //only when empty & stop
                    consumed_success.fetch_add(1);
                }
            }
        );
    }

    //STEP2: create producer, push item
    for (int i=0; i<num_producer; ++i) {
        // if que full, producer keep try push
        // copy i variable
        producers.emplace_back(
            [&que, i, &produced_success, data_per_producer]()      
            {
                for (int j=0; j<data_per_producer; ++j) {
                    int data = i * data_per_producer + j;   //value is asscending
                    // if full: keep trying
                    while (true) {
                        JobQueue<int>::SubmitResult result = que.try_push(std::move(data));
                        if (result == JobQueue<int>::SubmitResult::Pushed) {
                            produced_success.fetch_add(1);
                            break;
                        }
                        if (result == JobQueue<int>::SubmitResult::Shutdown) {
                            // EXPECT_FALSE(1);        //ERROR: should not shutdown here
                            return;     //break while and leave for loop
                        }
                        // FULL: continue while loop
                        std::this_thread::yield();  //better than sleep (1ms)
                    }
                }
            }
        );
    }
    //produced all data
    for (int i=0; i<static_cast<int>(producers.size());++i) {
        if(producers[i].joinable())        //should be joinable
            producers[i].join();
    }

    // STEP3: shutdown: expect consumer consume all data then join
    que.shutdown();

    for (int i=0; i<static_cast<int>(consumers.size()); ++i) {
        if (consumers[i].joinable())
            consumers[i].join();
    }

    // STEP4: check results
    EXPECT_EQ(produced_success.load(), total_data);
    EXPECT_EQ(consumed_success.load(), produced_success.load());

    EXPECT_EQ(produced_success.load(), m.submitted_total.load());
    EXPECT_EQ(m.queued_number.load(), 0u);

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
            if (passed)
                count_passed++;
            else
                count_failed++;

            std::cerr << "[" << result[passed] << "] " << name << "\n";
        };

    

    // single-thread
    run_test(test_policy_fifo, "test_policy_fifo");
    run_test(test_policy_lifo, "test_policy_lifo");
    run_test(test_trypush_full, "test_trypush_full");
    run_test(test_trypush_after_shutdown, "test_trypush_after_shutdown");
    run_test(test_trypush_priority_shutdown_full, "test_trypush_priority_shutdown_full");
    run_test(test_pop_empty_shutdown, "test_pop_empty_shutdown");
    run_test(test_pop_shutdown_drain_lifo, "test_pop_shutdown_drain_lifo");
    run_test(test_pop_shutdown_drain_fifo, "test_pop_shutdown_drain_fifo");

    // multi-thread / concurrency
    run_test(test_pop_wait_push, "test_pop_wait_push");
    run_test(test_pop_shutdown, "test_pop_shutdown");
    run_test(test_pop_wake_all_waiting_by_shutdown, "test_pop_wake_all_waiting_by_shutdown");
    run_test(test_concurrency_stress, "test_concurrency_stress");

    std::cerr << "-----------------------------\n";
    std::cerr << "TOTAL : " << count_total << "\n";
    std::cerr << "PASS  : " << count_passed << "\n";
    std::cerr << "FAIL  : " << count_failed << "\n";

    // return 0;
    return (count_failed==0) ? 0 : 1;           //TSAN/ASAN
}


