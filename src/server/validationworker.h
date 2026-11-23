#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <stdexcept>
namespace tak::srv {
// One validation at a time, two waiting jobs. Tasks own immutable input and must
// not capture connections, rooms, or VFS objects owned by the network thread.
class ValidationWorker {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> queue_;
    bool stopping_=false;
    std::thread thread_;
public:
    ValidationWorker():thread_([this] {
        for(;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock,[this]{return stopping_ || !queue_.empty();});
                if(stopping_)return;
                job=std::move(queue_.front());queue_.pop_front();
            }
            job();
        }
    }){}
    ~ValidationWorker() {
        {std::lock_guard lock(mutex_);stopping_=true;queue_.clear();}
        ready_.notify_one();thread_.join();
    }
    template<class F> auto submit(F fn) {
        auto task=std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::move(fn));
        auto future=task->get_future();
        {std::lock_guard lock(mutex_);
            if(stopping_ || queue_.size()>=2)throw std::runtime_error("map validation queue full; retry later");
            queue_.push_back([task]{(*task)();});
        }
        ready_.notify_one();return future;
    }
};
}
