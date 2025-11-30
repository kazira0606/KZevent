#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/sys_error.hpp"

namespace kzevent::core {
/* EventType运算符重载 */
constexpr EventType operator|(EventType a, EventType b) noexcept {
  return static_cast<EventType>(static_cast<uint32_t>(a) |
                                static_cast<uint32_t>(b));
}

constexpr EventType operator&(EventType a, EventType b) noexcept {
  return static_cast<EventType>(static_cast<uint32_t>(a) &
                                static_cast<uint32_t>(b));
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

/* 事件类型位转换表 */
template <typename T> struct EventToLocal {
  T kzevent_type;
  uint32_t local_type;
};

constexpr EventToLocal<EventType> kEventTypeToEpoll[] = {
    {EventType::kRead, static_cast<uint32_t>(EPOLLIN)},
    {EventType::kWrite, static_cast<uint32_t>(EPOLLOUT)},
    {EventType::kError, static_cast<uint32_t>(EPOLLERR)},
    {EventType::kHangUp, static_cast<uint32_t>(EPOLLHUP)}};

constexpr EventToLocal<EventMode> kEventModeToEpoll[] = {
    {EventMode::kET, static_cast<uint32_t>(EPOLLET)},
    {EventMode::kOneShot, static_cast<uint32_t>(EPOLLONESHOT)}};

/* 事件类型转换 */
namespace {
constexpr uint32_t kz_to_local(const EventType events) {
  uint32_t local_events = 0;

  for (const auto &[type, local_type] : kEventTypeToEpoll) {
    if ((events & type) != EventType::kNone) {
      local_events |= local_type;
    }
  }
  return local_events;
}

constexpr uint32_t kz_to_local(const EventMode modes) {
  uint32_t local_modes = 0;

  for (const auto &[mode, local_mode] : kEventModeToEpoll) {
    if ((modes & mode) != EventMode::kNone) {
      local_modes |= local_mode;
    }
  }
  return local_modes;
}

constexpr EventType local_to_kz(const uint32_t local_events) {
  auto events = EventType::kNone;

  for (const auto &[type, local_type] : kEventTypeToEpoll) {
    if ((local_events & local_type) != 0) {
      events = events | type;
    }
  }
  return events;
}
} // namespace

/* Loop类 */
Loop::Loop() {
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    sys_error::fatal();
  }

  io_wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (io_wake_fd_ < 0) {
    sys_error::fatal();
  }

  epoll_event io_wake_ev{};
  io_wake_ev.data.fd = io_wake_fd_;
  io_wake_ev.events = EPOLLIN | EPOLLET;
  if (const auto ret =
          epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, io_wake_fd_, &io_wake_ev);
      ret < 0) {
    sys_error::fatal();
  }

  KZ_LOG_INFO("loop create successfully!");
}

Loop::~Loop() {
  /* 禁止在executor线程析构 */
  if (std::this_thread::get_id() == io_thread_.get_id() ||
      std::this_thread::get_id() == heavy_thread_.get_id()) {
    KZ_LOG_FATAL("~Loop() called from executor thread!");
    std::terminate();
  }

  try {
    stop();
  } catch (const std::exception &e) {
    sys_error::error(e);
  }

  /* 清理由于executor线程投递stop导致的残留joint */
  if (io_thread_.joinable()) {
    io_thread_.join();
  }

  if (heavy_thread_.joinable()) {
    heavy_thread_.join();
  }

  if (const auto ret = close(epoll_fd_); ret < 0) {
    sys_error::error();
  }

  if (const auto ret = close(io_wake_fd_); ret < 0) {
    sys_error::error();
  }

  for (const auto &[fd, event] : events_) {
    if (const auto ret = close(fd); ret < 0) {
      sys_error::error();
    }
  }

  KZ_LOG_INFO("loop delete successfully!");
}

void Loop::start() {
  /* 同时启动io_thread和heavy_thread */
  if (auto expected{ExeState::Stopped};
      !exe_state_.compare_exchange_strong(expected, ExeState::Running)) {
    KZ_LOG_INFO("loop has started executor or loop is stopping executor!");
    return;
  }

  /* 清理由于run in loop投递stop导致的残留joint */
  if (io_thread_.joinable()) {
    if (std::this_thread::get_id() == io_thread_.get_id()) {
      KZ_LOG_ERROR("start() called from executor thread!");
      return;
    }
    io_thread_.join();
  }

  if (heavy_thread_.joinable()) {
    if (std::this_thread::get_id() == heavy_thread_.get_id()) {
      KZ_LOG_ERROR("start() called from executor thread!");
      return;
    }
    heavy_thread_.join();
  }

  /* 同时创建io_thread和heavy_thread */
  try {
    io_thread_ = std::thread([this] { io_executor(); });
    heavy_thread_ = std::thread([this] { heavy_executor(); });
  } catch (const std::exception &e) {
    exe_state_.store(ExeState::Stopped);
    sys_error::fatal(e);
  }

  KZ_LOG_INFO("loop executor start successfully!");
}

void Loop::stop() {
  if (auto expected{ExeState::Running};
      !exe_state_.compare_exchange_strong(expected, ExeState::Stopping)) {
    KZ_LOG_INFO("loop executor has stopped or executor is stopping!");
  }

  /* 防止在executor中投递stop导致self joint */
  if (io_thread_.joinable()) {
    if (std::this_thread::get_id() == io_thread_.get_id()) {
      return;
    }
    /* 唤醒io executor */
    wake_io_up();
    io_thread_.join();
  }

  if (heavy_thread_.joinable()) {
    if (std::this_thread::get_id() == heavy_thread_.get_id()) {
      return;
    }
    /* 唤醒heavy executor */
    wake_heavy_up();
    io_thread_.join();
  }
}

void Loop::post_io_task(LoopTask task) {
  if (std::this_thread::get_id() == io_thread_.get_id()) {
    task();
  } else {
    add_io_task(std::move(task));
  }
}

void Loop::post_heavy_task(LoopTask task) {
  if (std::this_thread::get_id() == heavy_thread_.get_id()) {
    task();
  } else {
    add_heavy_task(std::move(task));
  }
}

void Loop::register_fd(int32_t fd) {
  auto task = [this, fd] {
    if (const auto it = events_.find(fd); it != events_.end()) {
      KZ_LOG_INFO("fd is registered repeatedly!");
    } else {
      /* 添加注册 */
      events_.emplace(fd, Event{});
    }
  };

  if (std::this_thread::get_id() == io_thread_.get_id()) {
    task();
  } else {
    add_io_task(std::move(task));
  }
}

void Loop::unregister_fd(int32_t fd) {
  auto task = [this, fd] {
    if (const auto it = events_.find(fd); it == events_.end()) {
      KZ_LOG_INFO("fd is unregistered repeatedly!");
    } else {
      if (it->second.in_epoll == true) {
        if (const auto ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            ret < 0 && errno != ENOENT) {
          sys_error::error();
        }
      }
      /* 取消注册 */
      events_.erase(it);
      if (const auto ret = close(fd); ret < 0) {
        sys_error::error();
      }
    }
  };

  if (std::this_thread::get_id() == io_thread_.get_id()) {
    task();
  } else {
    add_io_task(std::move(task));
  }
}

void Loop::enable_fd(const int32_t fd, const EventType types,
                     const EventMode modes, CallBack cb) {
  auto task = [this, fd, types, modes, cb = std::move(cb)]() mutable {
    if (const auto it = events_.find(fd); it == events_.end()) {
      KZ_LOG_INFO("fd has not registered!");
    } else {
      int epoll_op{};
      it->second.in_epoll ? epoll_op = EPOLL_CTL_MOD : epoll_op = EPOLL_CTL_ADD;

      epoll_event ev{};
      ev.events = kz_to_local(types) | kz_to_local(modes);
      ev.data.fd = fd;

      if (epoll_ctl(epoll_fd_, epoll_op, fd, &ev) < 0) {
        if (epoll_op == EPOLL_CTL_ADD && errno == EEXIST) {
          if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            sys_error::error();
            return;
          }
        } else if (epoll_op == EPOLL_CTL_MOD && errno == ENOENT) {
          if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            sys_error::error();
            return;
          }
        } else {
          sys_error::error();
          return;
        }
      }
      it->second.in_epoll = true;
      it->second.cb_ = std::move(cb);
    }
  };

  if (std::this_thread::get_id() == io_thread_.get_id()) {
    task();
  } else {
    add_io_task(std::move(task));
  }
}

void Loop::disable_fd(int32_t fd) {
  auto task = [this, fd] {
    if (const auto it = events_.find(fd); it == events_.end()) {
      KZ_LOG_INFO("fd has not registered!");
    } else {
      if (const auto ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
          ret < 0 && errno != ENOENT) {
        sys_error::error();
        return;
      }
      it->second.in_epoll = false;
      it->second.cb_ = [](EventType) {};
    }
  };

  if (std::this_thread::get_id() == io_thread_.get_id()) {
    task();
  } else {
    add_io_task(std::move(task));
  }
}

void Loop::wake_io_up() const {
  constexpr uint64_t val = 1;
  if (const auto ret = write(io_wake_fd_, &val, sizeof(val)); ret < 0) {
    sys_error::error();
  }
}

void Loop::wake_heavy_up() { heavy_wake_cv_.notify_one(); }

void Loop::io_executor() {
  std::array<epoll_event, kEpollMaxLen> evs{};

  while (exe_state_.load() == ExeState::Running) {
    const auto ev_count = epoll_wait(epoll_fd_, evs.data(), kEpollMaxLen, -1);
    if (ev_count < 0) {
      if (errno == EINTR) {
        /* 内核中断，重新wait */
        continue;
      }
      sys_error::fatal();
    }

    /* 扫描文件描述符 */
    for (auto it = evs.begin(); it != (evs.begin() + ev_count); it += 1) {
      if (it->data.fd == io_wake_fd_) {
        uint64_t val;
        if (const auto ret = read(io_wake_fd_, &val, sizeof(val)); ret < 0) {
          sys_error::error();
        }

        /* 执行投递任务 */
        {
          std::lock_guard lock(io_queue_mtx_);
          std::swap(io_exe_queue_, io_buffer_queue_);
        }
        for (const auto &task : io_exe_queue_) {
          task();
        }
        io_exe_queue_.clear();
        continue;
      }

      /* 执行回调 */
      if (const auto event_it = events_.find(it->data.fd);
          event_it != events_.end()) {
        const auto cb = event_it->second.cb_;
        cb(local_to_kz(it->events));
      }
    }
  }

  if (auto expected{ExeState::Stopping};
      !exe_state_.compare_exchange_strong(expected, ExeState::Stopped)) {
    KZ_LOG_ERROR("loop io executor stop failed!");
  }
  KZ_LOG_INFO("loop io executor stop successfully!");
}

void Loop::heavy_executor() {
  while (exe_state_.load() == ExeState::Running) {
    std::unique_lock wait_lock(heavy_queue_mtx_);
    {
      heavy_wake_cv_.wait(wait_lock, [this] {
        return !heavy_buffer_queue_.empty() ||
               exe_state_.load() != ExeState::Running;
      });
      std::swap(heavy_exe_queue_, heavy_buffer_queue_);
    }
    wait_lock.unlock();

    for (const auto &task : heavy_exe_queue_) {
      task();
    }
    heavy_exe_queue_.clear();
  }

  if (auto expected{ExeState::Stopping};
      !exe_state_.compare_exchange_strong(expected, ExeState::Stopped)) {
    KZ_LOG_ERROR("loop heavy executor stop failed!");
  }
  KZ_LOG_INFO("loop heavy executor stop successfully!");
}

/* FdHandler类 */
FdHandler::FdHandler(Loop &loop, const int32_t fd) : in_loop_(&loop), fd_(fd) {
  in_loop_->register_fd(fd);
}

FdHandler::~FdHandler() {
  if (fd_ != -1 && in_loop_ != nullptr) {
    in_loop_->unregister_fd(fd_);
  }
}

FdHandler::FdHandler(FdHandler &&other) noexcept { swap(other); }

FdHandler &FdHandler::operator=(FdHandler other) {
  swap(other);
  return *this;
}

void FdHandler::swap(FdHandler &other) noexcept {
  std::swap(fd_, other.fd_);
  std::swap(in_loop_, other.in_loop_);
}

void FdHandler::update_event(LifeChecker life_checker, const EventType types,
                             const EventMode modes, CallBack cb) const {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的handler */
    KZ_LOG_ERROR("event handler fd has closed!");
    return;
  }

  auto wrapper_cb = [life_checker = std::move(life_checker),
                     cb = std::move(cb)](const EventType event_types) {
    if (!life_checker.lock()) {
      KZ_LOG_ERROR("executor can`t run callback! fd has closed!");
      return;
    }
    cb(event_types);
  };

  in_loop_->enable_fd(fd_, types, modes, wrapper_cb);
}

void FdHandler::disable_event() const {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的handler */
    KZ_LOG_ERROR("event handler fd has closed!");
    return;
  }

  in_loop_->disable_fd(fd_);
}
} // namespace kzevent::core
