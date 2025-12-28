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
  kShutdown = 1U << 3,
};

/* 事件模式（强制ET） */
enum class EventMode : uint32_t {
  kDefault = 0,
  kOneShot = 1U << 0,
};

/* EventType运算符重载 */
constexpr EventType operator|(EventType a, EventType b) noexcept {
  return static_cast<EventType>(static_cast<uint32_t>(a) |
                                static_cast<uint32_t>(b));
}

constexpr EventType operator&(EventType a, EventType b) noexcept {
  return static_cast<EventType>(static_cast<uint32_t>(a) &
                                static_cast<uint32_t>(b));
}

constexpr EventType operator~(EventType a) noexcept {
  return static_cast<EventType>(~static_cast<uint32_t>(a));
}

/* EventMode运算符重载  */
constexpr EventMode operator|(EventMode a, EventMode b) noexcept {
  return static_cast<EventMode>(static_cast<uint32_t>(a) |
                                static_cast<uint32_t>(b));
}

constexpr EventMode operator&(EventMode a, EventMode b) noexcept {
  return static_cast<EventMode>(static_cast<uint32_t>(a) &
                                static_cast<uint32_t>(b));
}

constexpr EventMode operator~(EventMode a) noexcept {
  return static_cast<EventMode>(~static_cast<uint32_t>(a));
}

using CallBack = std::function<void(const EventType)>;
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

  bool start();

  void stop();

private:
  /* 事件 */
  struct Event {
    bool in_epoll{false};
    CallBack cb_{[](const EventType) {}};
  };

  /* loop fd 接口 */
  void register_fd(int32_t fd);

  void unregister_fd(int32_t fd);

  void enable_fd(int32_t fd, EventType types, EventMode modes, CallBack cb);

  void enable_fd(int32_t fd, EventType types, EventMode modes);

  void disable_fd(int32_t fd);

  /* 提交loop任务 */
  void wake_io_up() const;

  void wake_heavy_up();

  void post_io_task(LoopTask task);

  void post_heavy_task(LoopTask task);

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

  friend void swap(LoopChannel &a, LoopChannel &b) noexcept;

  template <typename Fun>
  void update_event(LifeChecker life_checker, EventType types, EventMode modes,
                    Fun cb);

  void update_event(EventType types, EventMode modes);

  [[nodiscard]] std::pair<EventType, EventMode> get_event_info() const;

  void disable_event() const;

  template <typename Fun>
  void post_io_task(LifeChecker life_checker, Fun task) const;

  template <typename Fun>
  void post_heavy_task(LifeChecker life_checker, Fun task) const;

  [[nodiscard]] int32_t get_fd() const;

  [[nodiscard]] Loop *get_loop() const;

private:
  Loop *in_loop_{nullptr};
  int32_t fd_{-1};
  EventType event_types_{EventType::kDefault};
  EventMode event_modes_{EventMode::kDefault};
};

/*-------------------- 模板实现 --------------------*/
template <typename Fun>
void LoopChannel::update_event(LifeChecker life_checker, const EventType types,
                               const EventMode modes, Fun cb) {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的channel不允许访问 */
    KZ_LOG_ERROR("invalid channel be accessed!");
    return;
  }

  auto wrapper_cb = [life_checker = std::move(life_checker),
                     cb = std::move(cb)](const EventType event_types) mutable {
    const auto keep_life{life_checker.lock()};
    if (!keep_life) {
      KZ_LOG_ERROR("executor can`t run callback! fd has closed!");
      return;
    }
    cb(event_types);
  };

  in_loop_->enable_fd(fd_, types, modes, std::move(wrapper_cb));
  event_types_ = types;
  event_modes_ = modes;
}

template <typename Fun>
void LoopChannel::post_io_task(LifeChecker life_checker, Fun task) const {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的channel不允许访问 */
    KZ_LOG_ERROR("invalid channel be accessed!");
    return;
  }

  auto wrapper_task = [life_checker = std::move(life_checker),
                       task = std::move(task)]() mutable {
    const auto keep_life{life_checker.lock()};
    if (!keep_life) {
      return;
    }
    task();
  };

  in_loop_->post_io_task(std::move(wrapper_task));
}

template <typename Fun>
void LoopChannel::post_heavy_task(LifeChecker life_checker, Fun task) const {
  {
    if (fd_ == -1 || in_loop_ == nullptr) {
      /* 无效的channel不允许访问 */
      KZ_LOG_ERROR("invalid channel be accessed!");
      return;
    }

    auto wrapper_task = [life_checker = std::move(life_checker),
                         task = std::move(task)]() mutable {
      const auto keep_life{life_checker.lock()};
      if (!keep_life) {
        return;
      }
      task();
    };

    in_loop_->post_heavy_task(std::move(wrapper_task));
  }
}
} // namespace kzevent::core