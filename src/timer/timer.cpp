#include <atomic>
#include <cstdint>
#include <optional>

#include <sys/timerfd.h>
#include <unistd.h>

#include "kzevent/core.hpp"
#include "kzevent/sys_error.hpp"
#include "kzevent/timer/timer.hpp"

namespace kzevent::timer {
/* TimerUnit重载 */
bool TimerUnit::operator<(const TimerUnit &other) const noexcept {
  return this->time_stamp_ < other.time_stamp_;
}

bool TimerUnit::operator>(const TimerUnit &other) const noexcept {
  return this->time_stamp_ > other.time_stamp_;
}

namespace {
[[nodiscard]] std::optional<core::LoopChannel>
make_timer_channel(core::Loop &loop) {
  const auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }

  /* 立即移交fd便于RAII */
  core::LoopChannel timer_channel(loop, fd);

  return timer_channel;
}
} // namespace

/*-------------------- 定时器  --------------------*/
Timer::~Timer() { stop(); }

std::shared_ptr<Timer> Timer::make_timer(core::Loop &loop) {
  struct EnableMakeShared : public Timer {
    explicit EnableMakeShared(core::Loop &loop) : Timer(loop) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop);
  ret->start();
  return ret;
}

uint64_t Timer::post_once_task(TimerCallBack cb, CallBackOwner owner,
                               uint64_t timeout_ms) {
  const auto id = ++timer_id_start_;

  auto task = [this, cb = std::move(cb), owner = std::move(owner), timeout_ms,
               id]() mutable {
    /* 计算新任务时间戳 */
    const auto new_time_stamp = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(timeout_ms);

    /* 非周期任务 repeat_interval_ 为 0 */
    timer_queue_.push({std::move(cb), std::move(owner), new_time_stamp,
                       std::chrono::milliseconds(0), id});

    if (timer_queue_.top().time_stamp_ == new_time_stamp) {
      /* 新事件被插入了堆顶/新事件和堆顶时间相同 */
      set_next_wake_time(timer_queue_.top().time_stamp_);
    }
  };

  timer_channel_.post_io_task(weak_from_this(), std::move(task));
  return id;
}

uint64_t Timer::post_repeat_task(TimerCallBack cb, CallBackOwner owner,
                                 uint64_t timeout_ms) {
  const auto id = ++timer_id_start_;

  auto task = [this, cb = std::move(cb), owner = std::move(owner), timeout_ms,
               id]() mutable {
    /* 计算新任务时间戳 */
    const auto repeat_interval = std::chrono::milliseconds(timeout_ms);
    const auto new_time_stamp =
        std::chrono::steady_clock::now() + repeat_interval;

    /* 周期任务 repeat_interval_ 为 repeat_interval */
    timer_queue_.push(
        {std::move(cb), std::move(owner), new_time_stamp, repeat_interval, id});

    if (timer_queue_.top().time_stamp_ == new_time_stamp) {
      /* 新事件被插入了堆顶/新事件和堆顶时间相同 */
      set_next_wake_time(timer_queue_.top().time_stamp_);
    }
  };

  timer_channel_.post_io_task(weak_from_this(), std::move(task));
  return id;
}

void Timer::cancel_task(uint64_t timer_id) {
  /* 懒删除，注册删除id，执行时检测 */
  auto task = [this, timer_id] { delete_timer_ids_.emplace(timer_id); };

  timer_channel_.post_io_task(weak_from_this(), std::move(task));
}

Timer::Timer(core::Loop &loop)
    : timer_channel_([&] {
        auto ch = make_timer_channel(loop);
        if (!ch.has_value()) {
          throw std::runtime_error("make timer channel failed");
        }
        return std::move(ch).value();
      }()) {}

void Timer::set_next_wake_time(std::chrono::steady_clock::time_point time) {
  const auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              time - std::chrono::steady_clock::now())
                              .count();

  time_t sec{};
  long nsec{};

  if (timeout_ms <= 0) {
    /* 已超时，立即唤醒 */
    sec = 0;
    nsec = 1;
  } else {
    sec = timeout_ms / 1000;
    nsec = timeout_ms % 1000 * 1000000;
  }

  struct itimerspec ts {};
  /* 首次延迟 */
  ts.it_value.tv_sec = static_cast<time_t>(sec);
  ts.it_value.tv_nsec = static_cast<long>(nsec);

  if (const auto ret =
          timerfd_settime(timer_channel_.get_fd(), 0, &ts, nullptr);
      ret == -1) {
    sys_error::error();
    return;
  }
}

void Timer::start() {
  bool expected{false};
  if (!started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto task = [this](const core::EventType) {
    uint64_t val;
    while (read(timer_channel_.get_fd(), &val, sizeof(val)) > 0)
      ;
    if (errno != EAGAIN) {
      sys_error::fatal();
    }

    if (timer_queue_.empty()) {
      /* 无事件可执行 */
      return;
    }

    auto now = std::chrono::steady_clock::now();
    while (!timer_queue_.empty()) {
      const auto &timer = timer_queue_.top();

      if (timer.time_stamp_ > now) {
        /* 触发事件均执行完毕 */
        set_next_wake_time(timer_queue_.top().time_stamp_);
        return;
      }

      if (delete_timer_ids_.find(timer.timer_id_) != delete_timer_ids_.end()) {
        /* 是已注册删除的事件 */
        /* 移除事件 */
        delete_timer_ids_.erase(timer.timer_id_);
        timer_queue_.pop();
        continue;
      }

      /* 提取执行事件 */
      TimerUnit current_timer{
          std::move(const_cast<TimerCallBack &>(timer.cb_)),
          std::move(const_cast<CallBackOwner &>(timer.owner_)),
          timer.time_stamp_, timer.repeat_interval_, timer.timer_id_};

      /* 从堆中移除事件 */
      timer_queue_.pop();

      if (current_timer.repeat_interval_ != std::chrono::milliseconds(0)) {
        /* 周期任务 */
        /* 计算新时间戳 */
        const auto new_time_stamp =
            std::chrono::steady_clock::now() + current_timer.repeat_interval_;

        /* 再投递 */
        timer_queue_.push({current_timer.cb_, current_timer.owner_,
                           new_time_stamp, current_timer.repeat_interval_,
                           current_timer.timer_id_});
      }

      /* 执行事件 */
      timer_channel_.post_heavy_task(std::move(current_timer.owner_),
                                     std::move(current_timer.cb_));
    }
  };

  timer_channel_.update_event(weak_from_this(), core::EventType::kRead,
                              core::EventMode::kDefault, std::move(task));
}

void Timer::stop() {
  started_ = false;
  timer_channel_.disable_event();
}
} // namespace kzevent::timer