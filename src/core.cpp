#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <functional>
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
/* 事件类型位转换表 */
template <typename T> struct EventToLocal {
  T kzevent_type;
  uint32_t local_type;
};

constexpr EventToLocal<EventType> kEventTypeToEpoll[] = {
    {EventType::kRead, static_cast<uint32_t>(EPOLLIN)},
    {EventType::kWrite, static_cast<uint32_t>(EPOLLOUT)},
    {EventType::kError, static_cast<uint32_t>(EPOLLERR)},
    {EventType::kShutdown, static_cast<uint32_t>(EPOLLRDHUP)}};

constexpr EventToLocal<EventMode> kEventModeToEpoll[] = {
    {EventMode::kOneShot, static_cast<uint32_t>(EPOLLONESHOT)}};

/* 事件类型转换 */
namespace {
constexpr uint32_t kz_to_local(const EventType events) {
  uint32_t local_events{0};

  for (const auto &[type, local_type] : kEventTypeToEpoll) {
    if ((events & type) != EventType::kDefault) {
      local_events |= local_type;
    }
  }

  assert((local_events != 0) && "epoll must have at least one event type");

  return local_events;
}

constexpr uint32_t kz_to_local(const EventMode modes) {
  /* 强制使用边缘触发 */
  uint32_t local_modes{static_cast<uint32_t>(EPOLLET)};

  for (const auto &[mode, local_mode] : kEventModeToEpoll) {
    if ((modes & mode) != EventMode::kDefault) {
      local_modes |= local_mode;
    }
  }

  assert(((local_modes & EPOLLET) == EPOLLET) &&
         "epoll default must use lt mode");

  return local_modes;
}

constexpr EventType local_to_kz(const uint32_t local_events) {
  auto events{EventType::kDefault};

  for (const auto &[type, local_type] : kEventTypeToEpoll) {
    if ((local_events & local_type) != 0) {
      events = events | type;
    }
  }
  return events;
}
} // namespace

/*-------------------- 循环监听+执行事件 --------------------*/
Loop::Loop() {
  if (epoll_fd_ = epoll_create1(EPOLL_CLOEXEC); epoll_fd_ < 0) {
    sys_error::fatal();
  }

  if (io_wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); io_wake_fd_ < 0) {
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
  assert((std::this_thread::get_id() != io_thread_.get_id() &&
          std::this_thread::get_id() != heavy_thread_.get_id()) &&
         "loop cannot be destructed in executor thread!");

  stop();

  /* 清理由于executor线程投递stop导致的残留joint */
  if (io_thread_.joinable()) {
    io_thread_.join();
    exe_state_ = ExeState::Stopped;
  }

  if (heavy_thread_.joinable()) {
    heavy_thread_.join();
    exe_state_ = ExeState::Stopped;
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

bool Loop::start() {
  /* 清理由于run in loop投递stop导致的残留joint */
  if (io_thread_.joinable()) {
    if (std::this_thread::get_id() == io_thread_.get_id()) {
      KZ_LOG_ERROR("start() called from executor thread!");
      return false;
    }
    io_thread_.join();
    exe_state_ = ExeState::Stopped;
  }

  if (heavy_thread_.joinable()) {
    if (std::this_thread::get_id() == heavy_thread_.get_id()) {
      KZ_LOG_ERROR("start() called from executor thread!");
      return false;
    }
    heavy_thread_.join();
    exe_state_ = ExeState::Stopped;
  }

  /* 同时启动io_thread和heavy_thread */
  if (auto expected{ExeState::Stopped};
      !exe_state_.compare_exchange_strong(expected, ExeState::Running)) {
    KZ_LOG_INFO("loop has started executor or loop is stopping executor!");
    return false;
  }

  /* 同时创建io_thread和heavy_thread */
  io_thread_ = std::thread{[this] { io_executor(); }};
  heavy_thread_ = std::thread{[this] { heavy_executor(); }};

  KZ_LOG_INFO("loop executor start successfully!");

  return true;
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
    heavy_thread_.join();
  }

  exe_state_ = ExeState::Stopped;
}

void Loop::register_fd(int32_t fd) {
  auto task = [this, fd] {
    if (const auto [it, success] = events_.try_emplace(fd, Event{}); !success) {
      KZ_LOG_INFO("fd is registered repeatedly!");
    }
  };

  post_io_task(std::move(task));
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

  post_io_task(std::move(task));
}

void Loop::enable_fd(const int32_t fd, const EventType types,
                     const EventMode modes, CallBack cb) {
  auto task = [this, fd, types, modes, cb = std::move(cb)]() mutable {
    if (const auto it = events_.find(fd); it == events_.end()) {
      KZ_LOG_INFO("fd has not registered!");
    } else {
      int32_t epoll_op{};
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

  post_io_task(std::move(task));
}

void Loop::enable_fd(const int32_t fd, const EventType types,
                     const EventMode modes) {
  auto task = [this, fd, types, modes] {
    if (const auto it = events_.find(fd); it == events_.end()) {
      KZ_LOG_INFO("fd has not registered!");
    } else {
      int32_t epoll_op{};
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
    }
  };

  post_io_task(std::move(task));
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

  post_io_task(std::move(task));
}

void Loop::wake_io_up() const {
  constexpr uint64_t val{1};
  if (const auto ret = write(io_wake_fd_, &val, sizeof(val)); ret < 0) {
    if (errno != EAGAIN) {
      sys_error::error();
    }
  }
}

void Loop::post_io_task(LoopTask task) {
  {
    std::lock_guard lock{io_queue_mtx_};
    io_buffer_queue_.emplace_back(std::move(task));
  }
  wake_io_up();
}

void Loop::post_heavy_task(LoopTask task) {
  {
    std::lock_guard lock{heavy_queue_mtx_};
    heavy_buffer_queue_.emplace_back(std::move(task));
  }
  wake_heavy_up();
}

void Loop::wake_heavy_up() { heavy_wake_cv_.notify_one(); }

void Loop::io_executor() {
  std::array<epoll_event, kEpollMaxWaitLen> evs{};

  while (exe_state_ == ExeState::Running) {
    const auto ev_count =
        epoll_wait(epoll_fd_, evs.data(), kEpollMaxWaitLen, -1);
    if (ev_count < 0) {
      if (errno == EINTR) {
        /* 内核中断，重新wait */
        continue;
      }
      sys_error::fatal();
    }

    /* 扫描文件描述符 */
    for (auto it{evs.begin()}; it != (evs.begin() + ev_count); it += 1) {
      if (it->data.fd == io_wake_fd_) {
        uint64_t val{};
        while (read(io_wake_fd_, &val, sizeof(val)) > 0)
          ;
        if (errno != EAGAIN) {
          sys_error::fatal();
        }

        /* 执行投递任务 */
        {
          std::lock_guard lock{io_queue_mtx_};
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
        event_it->second.cb_(local_to_kz(it->events));
      }
    }
  }

  KZ_LOG_INFO("loop io executor stop successfully!");
}

void Loop::heavy_executor() {
  while (exe_state_ == ExeState::Running) {
    std::unique_lock wait_lock{heavy_queue_mtx_};
    {
      heavy_wake_cv_.wait(wait_lock, [this] {
        return !heavy_buffer_queue_.empty() || exe_state_ != ExeState::Running;
      });
      std::swap(heavy_exe_queue_, heavy_buffer_queue_);
    }
    wait_lock.unlock();

    for (const auto &task : heavy_exe_queue_) {
      task();
    }
    heavy_exe_queue_.clear();
  }

  KZ_LOG_INFO("loop heavy executor stop successfully!");
}

/*-------------------- Loop接口 --------------------*/
LoopChannel::LoopChannel(Loop &loop, const int32_t fd)
    : in_loop_{&loop}, fd_{fd} {
  in_loop_->register_fd(fd);
}

LoopChannel::~LoopChannel() {
  if (fd_ != -1 && in_loop_ != nullptr) {
    in_loop_->unregister_fd(fd_);
  }
}

LoopChannel::LoopChannel(LoopChannel &&other) noexcept
    : in_loop_(other.in_loop_), fd_(other.fd_) {
  other.fd_ = -1;
  other.in_loop_ = nullptr;
}

LoopChannel &LoopChannel::operator=(LoopChannel &&other) noexcept {
  swap(*this, other);
  return *this;
}

void swap(LoopChannel &a, LoopChannel &b) noexcept {
  std::swap(a.fd_, b.fd_);
  std::swap(a.in_loop_, b.in_loop_);
}

void LoopChannel::update_event(EventType types, EventMode modes) {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的channel不允许访问 */
    KZ_LOG_ERROR("invalid channel be accessed!");
    return;
  }

  in_loop_->enable_fd(fd_, types, modes);
  event_types_ = types;
  event_modes_ = modes;
}

[[nodiscard]] std::pair<EventType, EventMode>
LoopChannel::get_event_info() const {
  return {event_types_, event_modes_};
}

void LoopChannel::disable_event() const {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的channel不允许访问 */
    KZ_LOG_ERROR("invalid channel be accessed!");
    return;
  }

  in_loop_->disable_fd(fd_);
}

int32_t LoopChannel::get_fd() const {
  if (fd_ == -1 || in_loop_ == nullptr) {
    /* 无效的channel不允许访问 */
    KZ_LOG_ERROR("invalid channel be accessed!");
    return -1;
  }

  return fd_;
}
} // namespace kzevent::core
