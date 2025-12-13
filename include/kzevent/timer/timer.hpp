#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <set>

#include "kzevent/core.hpp"

namespace kzevent::timer {
/* 定时器回调 */
using TimerCallBack = core::LoopTask;

/* 生命周期 */
using CallBackOwner = core::LifeChecker;

/* 定时器 */
struct TimerUnit {
  TimerCallBack cb_{};
  CallBackOwner owner_{};
  std::chrono::steady_clock::time_point time_stamp_{};
  std::chrono::steady_clock::duration repeat_interval_{};
  uint64_t timer_id_{};
  bool operator<(const TimerUnit &other) const noexcept;
  bool operator>(const TimerUnit &other) const noexcept;
};

/* 定时队列 */
using TimerQueue =
    std::priority_queue<TimerUnit, std::vector<TimerUnit>, std::greater<>>;

/*-------------------- 定时器  --------------------*/
class Timer : public std::enable_shared_from_this<Timer> {
public:
  ~Timer();

  /* 静态工厂 */
  static std::shared_ptr<Timer> make_timer(core::Loop &loop);

  /* 接口 */
  uint64_t post_once_task(TimerCallBack cb, CallBackOwner owner,
                          uint64_t timeout_ms);

  uint64_t post_repeat_task(TimerCallBack cb, CallBackOwner owner,
                            uint64_t timeout_ms);

  void cancel_task(uint64_t timer_id);

private:
  explicit Timer(core::Loop &loop);

  void start();

  void stop();

  void set_next_wake_time(std::chrono::steady_clock::time_point time);

  /* 最小堆执行队列 */
  TimerQueue timer_queue_{};

  /* 周期任务id管理 */
  std::atomic<uint64_t> timer_id_start_{0};

  /* 周期任务删除表 */
  std::set<uint64_t> delete_timer_ids_{};

  /* loop channel */
  core::LoopChannel timer_channel_;

  /* 启动标志 */
  std::atomic<bool> started_{false};
};
} // namespace kzevent::timer