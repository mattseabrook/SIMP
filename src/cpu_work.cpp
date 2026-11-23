#include "cpu_work.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
class Workers
{
  public:
    Workers(unsigned limit = 8, const wchar_t *label = L"SIMP CPU worker")
    {
        const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, limit);
        try
        {
            for (unsigned i = 0; i < count; ++i)
                threads.emplace_back([this, label] {
#ifdef _WIN32
                    SetThreadDescription(GetCurrentThread(), label);
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
#else
                    (void)label;
#endif
                    for (;;)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock lock(mutex);
                            ready.wait(lock, [this] { return stopping || !queue.empty(); });
                            if (stopping && queue.empty())
                                return;
                            task = std::move(queue.front());
                            queue.pop_front();
                            ++running;
                        }
                        task();
                        {
                            std::lock_guard lock(mutex);
                            --running;
                            if (!running && queue.empty())
                                idle.notify_all();
                        }
                    }
                });
        }
        catch (...)
        {
            {
                std::lock_guard lock(mutex);
                stopping = true;
            }
            ready.notify_all();
            for (auto &thread : threads)
                thread.join();
            throw;
        }
    }
    ~Workers()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        for (auto &thread : threads)
            thread.join();
    }
    void push(std::function<void()> work)
    {
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(work));
        }
        ready.notify_one();
    }
    void wait()
    {
        std::unique_lock lock(mutex);
        idle.wait(lock, [this] { return queue.empty() && !running; });
    }
    std::vector<std::thread> threads;

  private:
    std::mutex mutex;
    std::condition_variable ready, idle;
    std::deque<std::function<void()>> queue;
    unsigned running = 0;
    bool stopping = false;
};
Workers &workers()
{
    static Workers pool;
    return pool;
}
Workers &ioWorkers()
{
    static Workers pool(1, L"SIMP history I/O");
    return pool;
}
} // namespace
void enqueueCpuWork(std::function<void()> work)
{
    workers().push(std::move(work));
}
void enqueueIoWork(std::function<void()> work)
{
    ioWorkers().push(std::move(work));
}
void waitForCpuWork()
{
    workers().wait();
    ioWorkers().wait();
}
unsigned cpuWorkerCount()
{
    return unsigned(workers().threads.size());
}
void parallelCpuRange(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)> &fn)
{
    if (!count)
        return;
    grain = std::max<std::size_t>(1, grain);
    if (count <= grain || cpuWorkerCount() == 1)
    {
        fn(0, count);
        return;
    }
    struct Group
    {
        std::atomic<std::size_t> next{0}, remaining;
        std::size_t count, grain;
        std::function<void(std::size_t, std::size_t)> fn;
        std::mutex mutex;
        std::condition_variable done;
        std::exception_ptr error;
        Group(std::size_t n, std::size_t g, const decltype(fn) &f) : remaining(n), count(n), grain(g), fn(f)
        {
        }
        void run()
        {
            for (;;)
            {
                const auto begin = next.fetch_add(grain, std::memory_order_relaxed);
                if (begin >= count)
                    return;
                const auto end = std::min(count, begin + grain);
                try
                {
                    fn(begin, end);
                }
                catch (...)
                {
                    std::lock_guard lock(mutex);
                    if (!error)
                        error = std::current_exception();
                }
                if (remaining.fetch_sub(end - begin, std::memory_order_acq_rel) == end - begin)
                {
                    std::lock_guard lock(mutex);
                    done.notify_all();
                }
            }
        }
    };
    auto group = std::make_shared<Group>(count, grain, fn);
    const auto helpers = std::min<std::size_t>(cpuWorkerCount() - 1, (count - 1) / grain);
    std::exception_ptr submissionError;
    try
    {
        for (std::size_t i = 0; i < helpers; ++i)
            enqueueCpuWork([group] { group->run(); });
    }
    catch (...)
    {
        // Finish already submitted readers before their caller can unwind.
        submissionError = std::current_exception();
    }
    group->run();
    std::unique_lock lock(group->mutex);
    group->done.wait(lock, [&] { return group->remaining.load(std::memory_order_acquire) == 0; });
    if (group->error)
        std::rethrow_exception(group->error);
    if (submissionError)
        std::rethrow_exception(submissionError);
}
