#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <kzevent/log.hpp>

namespace kzevent::core {
/* epoll配置 */
static constexpr uint32_t kEpollMaxWaitLen = 256;

/* 事件类型 */
enum class EventType : uint32_t {
  kDefault = 0,
  kRead = 1U << 0,
  kWrite = 1U << 1,
  kError = 1U << 2,
  kHangUp = 1U << 3,
};

/* 事件模式（强制ET） */
enum class EventMode : uint32_t {
  kDefault = 0,
  kOneShot = 1U << 0,
};

using CallBack = std::function<void(EventType)>;
using LoopTask = std::function<void()>;
using LifeChecker = std::weak_ptr<void>;

/*-------------------- 循环监听+执行事件 --------------------*/
class Loop {
  friend class LoopChannel;

public:
  Loop();

  ~Loop();

  Loop(const Loop &) = delete;

  Loop(Loop &&) = delete;

  Loop &operator=(const Loop &) = delete;

  Loop &operator=(Loop &&) = delete;

  void start();

  void stop();

private:
  /* 事件 */
  struct Event {
    bool in_epoll{false};
    CallBack cb_{[](EventType) {}};
  };

  /* loop fd 接口 */
  void register_fd(int32_t fd);

  void unregister_fd(int32_t fd);

  void enable_fd(int32_t fd, EventType types, EventMode modes, CallBack cb);

  void disable_fd(int32_t fd);

  /* 提交loop任务 */
  void wake_io_up() const;

  void wake_heavy_up();

  template <typename Fun> void post_io_task(Fun &&task) {
    if (std::this_thread::get_id() == io_thread_.get_id()) {
      task();
    } else {
      std::lock_guard lock{io_queue_mtx_};
      io_buffer_queue_.emplace_back(std::forward<Fun>(task));
      wake_io_up();
    }
  }

  template <typename Fun> void post_heavy_task(Fun &&task) {
    if (std::this_thread::get_id() == heavy_thread_.get_id()) {
      task();
    } else {
      std::lock_guard lock{heavy_queue_mtx_};
      heavy_buffer_queue_.emplace_back(std::forward<Fun>(task));
      wake_heavy_up();
    }
  }

  /* Loop执行队列执行器 */
  void io_executor();

  void heavy_executor();

  /* executor状态机状态 */
  enum class ExeState : uint8_t { Stopped, Running, Stopping };

  std::atomic<ExeState> exe_state_{ExeState::Stopped};

  std::thread io_thread_{};
  std::thread heavy_thread_{};

  /* executor执行队列 */
  std::mutex io_queue_mtx_{};
  std::vector<LoopTask> io_exe_queue_{};
  std::vector<LoopTask> io_buffer_queue_{};
  int32_t io_wake_fd_{-1};
  int32_t epoll_fd_{-1};

  std::mutex heavy_queue_mtx_{};
  std::vector<LoopTask> heavy_exe_queue_{};
  std::vector<LoopTask> heavy_buffer_queue_{};
  std::condition_variable heavy_wake_cv_{};

  /* 持有的Events */
  std::unordered_map<int32_t, Event> events_{};
};

/*-------------------- Loop接口 --------------------*/
class LoopChannel {
public:
  LoopChannel() = delete;

  LoopChannel(Loop &loop, int32_t fd);

  ~LoopChannel();

  LoopChannel(const LoopChannel &) = delete;

  LoopChannel(LoopChannel &&other) noexcept;

  LoopChannel &operator=(const LoopChannel &other) = delete;

  LoopChannel &operator=(LoopChannel &&other) noexcept;

  void swap(LoopChannel &other) noexcept;

  void update_event(LifeChecker life_checker, EventType types, EventMode modes,
                    CallBack &cb) const;

  void update_event(LifeChecker life_checker, EventType types, EventMode modes,
                    CallBack &&cb) const;

  void disable_event() const;

  template <typename Fun>
  void post_io_task(LifeChecker life_checker, Fun &&task) const {
    if (fd_ == -1 || in_loop_ == nullptr) {
      /* 无效的channel不允许访问 */
      KZ_LOG_ERROR("invalid channel be accessed!");
      return;
    }

    auto task_wrapper = [life_checker = std::move(life_checker),
                         task = std::forward<Fun>(task)]() mutable {
      const auto keep_life{life_checker.lock()};
      if (!keep_life) {
        return;
      }
      task();
    };

    in_loop_->post_io_task(std::move(task_wrapper));
  }

  template <typename Fun>
  void post_heavy_task(LifeChecker life_checker, Fun &&task) const {
    if (fd_ == -1 || in_loop_ == nullptr) {
      /* 无效的channel不允许访问 */
      KZ_LOG_ERROR("invalid channel be accessed!");
      return;
    }

    auto task_wrapper = [life_checker = std::move(life_checker),
                         task = std::forward<Fun>(task)]() mutable {
      const auto keep_life{life_checker.lock()};
      if (!keep_life) {
        return;
      }
      task();
    };

    in_loop_->post_heavy_task(std::move(task_wrapper));
  }

  [[nodiscard]] int32_t get_fd() const;

private:
  Loop *in_loop_{nullptr};
  int32_t fd_{-1};
};
} // namespace kzevent::core
