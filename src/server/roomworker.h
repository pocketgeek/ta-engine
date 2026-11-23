#pragma once
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace tak::srv {
// One persistent worker per running room. The network thread submits one frozen
// tick and polls its future; it never joins a tick batch or waits for a busy room.
class RoomWorker {
public:
    RoomWorker():thread_([this] {
        for(;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock,[this] {return stop_ || bool(job_);});
                if(!job_ && stop_)return;
                job=std::move(job_);job_={};
            }
            job(); // packaged_task carries exceptions back to the network thread
        }
    }){}
    ~RoomWorker() {
        {std::lock_guard lock(mutex_);stop_=true;}
        ready_.notify_one();
        if(thread_.joinable())thread_.join();
    }
    RoomWorker(const RoomWorker&)=delete;
    RoomWorker& operator=(const RoomWorker&)=delete;
    template<class F> auto submit(F&& fn) {
        using Result=std::invoke_result_t<F>;
        auto task=std::make_shared<std::packaged_task<Result()>>(std::forward<F>(fn));
        auto future=task->get_future();
        {
            std::lock_guard lock(mutex_);
            if(stop_ || job_)throw std::logic_error("room worker already queued or stopped");
            job_=[task] {(*task)();};
        }
        ready_.notify_one();return future;
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::function<void()> job_;
    bool stop_=false;
    std::thread thread_;
};
}
