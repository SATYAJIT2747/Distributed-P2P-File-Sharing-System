#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <unistd.h>

class ThreadPool
{
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    std::atomic<bool> stop;
    std::atomic<int> active_tasks;

public:
    ThreadPool(size_t num_threads) : stop(false), active_tasks(0)
    {
        for (size_t i = 0; i < num_threads; ++i)
        {
            workers.emplace_back([this]
                                 {
                while(true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { 
                            return this->stop || !this->tasks.empty(); 
                        });
                        
                        if(this->stop && this->tasks.empty())
                            return;
                        
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    active_tasks++;
                    task();
                    active_tasks--;
                } });
        }
    }

    template <class F>
    void enqueue(F &&f)
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    void wait_until_idle()
    {
        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                if (tasks.empty() && active_tasks == 0)
                    return;
            }
            usleep(10000); // 10ms
        }
    }

    int get_active_count()
    {
        return active_tasks.load();
    }

    ~ThreadPool()
    {
        stop = true;
        condition.notify_all();
        for (std::thread &worker : workers)
            if (worker.joinable())
                worker.join();
    }
};

#endif // THREAD_POOL_H
