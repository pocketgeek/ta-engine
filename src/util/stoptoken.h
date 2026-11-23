#pragma once
#include <atomic>
#include <memory>
#include <utility>

namespace tak {
// Poll-only cancellation for SDKs without C++20 stop_token (macOS 14).
// Tokens retain their state after the connection/source is destroyed. No
// callbacks, thread ownership or connection pointers cross worker boundaries.
class StopSource;
class StopToken {
    std::shared_ptr<const std::atomic<bool>> stopped_;
    explicit StopToken(std::shared_ptr<const std::atomic<bool>> state):stopped_(std::move(state)){}
    friend class StopSource;
public:
    StopToken()=default;
    bool stop_requested() const noexcept {
        return stopped_ && stopped_->load(std::memory_order_acquire);
    }
};
class StopSource {
    std::shared_ptr<std::atomic<bool>> stopped_=std::make_shared<std::atomic<bool>>(false);
public:
    StopToken get_token() const noexcept {return StopToken(stopped_);}
    bool request_stop() noexcept {
        return stopped_ && !stopped_->exchange(true,std::memory_order_acq_rel);
    }
};
}
