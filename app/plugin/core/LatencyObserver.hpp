#pragma once
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <functional>

// Poll on the main queue: graph edits can originate on the HTTP or loader thread.
class LatencyObserver {
public:
    explicit LatencyObserver(std::function<void()> callback) {
        auto start = ^{
            timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(timer_, ^{ callback(); });
            dispatch_resume(timer_);
        };
        if (pthread_main_np()) start(); else dispatch_sync(dispatch_get_main_queue(), start);
    }
    ~LatencyObserver() {
        auto stop = ^{ if (timer_) { dispatch_source_cancel(timer_); timer_ = nullptr; } };
        if (pthread_main_np()) stop(); else dispatch_sync(dispatch_get_main_queue(), stop);
    }
private:
    dispatch_source_t timer_ = nullptr;
};
