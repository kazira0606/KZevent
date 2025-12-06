#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "kzevent/core.hpp"

using namespace kzevent::core;

/* 测试规模配置 */
#ifndef KZEVENT_TEST_THREAD_COUNT
#define KZEVENT_TEST_THREAD_COUNT 16
#endif

#ifndef KZEVENT_TEST_TASKS_PER_THREAD
#define KZEVENT_TEST_TASKS_PER_THREAD 50
#endif

#ifndef KZEVENT_TEST_DATA_SIZE
#define KZEVENT_TEST_DATA_SIZE (64 * 1024)
#endif

/* 测试参数 */
static constexpr int32_t kThreadCount{KZEVENT_TEST_THREAD_COUNT};
static constexpr int32_t kTasksPerThread{KZEVENT_TEST_TASKS_PER_THREAD};
static constexpr size_t kDataSize{KZEVENT_TEST_DATA_SIZE};

/* 工具函数->设置非阻塞fd */
static void set_nonblocking(const int32_t fd) {
  const auto flags = fcntl(fd, F_GETFL, 0);
  ASSERT_NE(flags, -1)
      << "fcntl(F_GETFL) failed: "
      << std::error_code(errno, std::system_category()).message();
  ASSERT_NE(fcntl(fd, F_SETFL, flags | O_NONBLOCK), -1)
      << "fcntl(F_SETFL) failed: "
      << std::error_code(errno, std::system_category()).message();
}

/* 测试函数->写所有数据 */
static bool write_all(const int32_t fd, const void *buf, size_t len) {
  const auto *p{static_cast<const char *>(buf)};
  size_t sent{0};
  while (sent < len) {
    const auto n = write(fd, p + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

/* 测试函数->读所有数据 */
static bool read_all(const int32_t fd, void *buf, size_t len) {
  auto *p{static_cast<char *>(buf)};
  size_t get{0};
  while (get < len) {
    const auto n = read(fd, p + get, len - get);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    get += static_cast<size_t>(n);
  }
  return true;
}

/* 测试函数->计算字符串中A的个数 */
static int32_t count_A(const std::string &string) {
  return static_cast<int32_t>(std::count(string.begin(), string.end(), 'A'));
}

/* 随机生成字符串 */
static std::string random_string(const size_t len) {
  static const std::string stringset{"ABCDEFGHIJKLMN"};
  static const std::vector<char> charset{stringset.begin(), stringset.end()};
  static thread_local std::mt19937 gen{std::random_device{}()};
  static thread_local std::uniform_int_distribution<int32_t> dis{
      0, static_cast<int32_t>(charset.size() - 1)};

  std::string ret{};
  ret.reserve(len);
  for (size_t i{0}; i < len; ++i)
    ret.push_back(charset[dis(gen)]);
  return ret;
}

/* 线程安全的记录第一个错误 */
struct FailState {
  std::atomic<bool> failed_{false};
  std::mutex mtx_{};
  std::string msg_{};

  void set(std::string msg) {
    if (bool expected{false}; failed_.compare_exchange_strong(expected, true)) {
      std::lock_guard lock{mtx_};
      msg_ = std::move(msg);
    }
  }
};

/* 测试类 */
class KZCoreTest : public testing::Test {
protected:
  void SetUp() override {
    /* 忽略 SIGPIPE->避免提前关闭fd的测试失败 */
    std::signal(SIGPIPE, SIG_IGN);
    loop_ = std::make_unique<Loop>();
    loop_->start();
  }

  void TearDown() override {
    loop_->stop();
    loop_.reset();
  }

  std::unique_ptr<Loop> loop_{};
};

/*-------------------- 多线程io+多线程heavy任务测试 --------------------*/
TEST_F(KZCoreTest, MultiThreadWithIoAndHeavyTask) {
  /* mock 持有LoopHandler的上层类 */
  class SessionTestIoHeavy
      : public std::enable_shared_from_this<SessionTestIoHeavy> {
  public:
    SessionTestIoHeavy(Loop &loop, const int32_t in_read_fd,
                       const int32_t out_write_fd, const size_t data_size,
                       std::shared_ptr<FailState> fail)
        : in_read_channel_{loop, in_read_fd},
          out_write_channel_{loop, out_write_fd}, data_size_{data_size},
          fail_{std::move(fail)} {
      token_.reserve(data_size_);
    }

    void run() {
      in_read_channel_.update_event(
          weak_from_this(), EventType::kRead, EventMode::kDefault,
          [this](const EventType event_types) { in_read_cb(event_types); });
    }

  private:
    void in_read_cb(EventType) {
      while (true) {
        std::array<char, 4096> buffer{};
        const auto n =
            read(in_read_channel_.get_fd(), buffer.data(), buffer.size());

        if (n > 0) {
          token_.append(buffer.data(), static_cast<size_t>(n));

          if (token_.size() == data_size_) {
            /* 投递 heavy 线程 */
            in_read_channel_.post_heavy_task(
                weak_from_this(),
                [this, token{std::move(token_)}] { compute_cb(token); });

            token_.clear();
            token_.reserve(data_size_);
          }
          continue;
        }

        if (n < 0 && errno == EINTR)
          continue;
        if (n < 0 && errno == EAGAIN)
          break;
        if (n == 0)
          break;

        /* 记录错误 */
        fail_->set(std::string("session read error: ") +
                   std::error_code(errno, std::system_category()).message());
        break;
      }
    }

    void compute_cb(const std::string &token) {
      /* 计算A的个数 */
      auto count = count_A(token);

      out_write_channel_.post_io_task(weak_from_this(), [this, count] {
        if (!write_all(out_write_channel_.get_fd(), &count, sizeof(count))) {
          /* 记录错误  */
          fail_->set(std::string("session write error: ") +
                     std::error_code(errno, std::system_category()).message());
        }
      });
    }

    LoopChannel in_read_channel_;
    LoopChannel out_write_channel_;

    size_t data_size_{0};
    std::shared_ptr<FailState> fail_{};
    std::string token_{};
  };

  /* 全局记录第一条错误，出现即终止测试 */
  auto fail = std::make_shared<FailState>();

  /* 预留资源 */
  std::vector<std::array<int32_t, 2>> in_pipe(kThreadCount);
  std::vector<std::array<int32_t, 2>> out_pipe(kThreadCount);

  std::vector<std::shared_ptr<SessionTestIoHeavy>> sessions{};
  sessions.reserve(kThreadCount);

  for (int32_t i{0}; i < kThreadCount; ++i) {
    /* 创建one session per thread */
    ASSERT_EQ(::pipe(in_pipe[i].data()), 0)
        << std::error_code(errno, std::system_category()).message();
    ASSERT_EQ(::pipe(out_pipe[i].data()), 0)
        << std::error_code(errno, std::system_category()).message();

    /* in_pipe session监听fd为非阻塞，其余降低复杂度均为阻塞 */
    set_nonblocking(in_pipe[i][0]);

    auto session = std::make_shared<SessionTestIoHeavy>(
        *loop_, in_pipe[i][0], out_pipe[i][1], kDataSize, fail);
    session->run();
    sessions.push_back(session);
  }

  /* 生成token和校验答案 */
  const auto payload = random_string(kDataSize);
  const auto expected_count = count_A(payload);

  std::vector<std::thread> clients{};
  clients.reserve(kThreadCount);

  std::atomic<int32_t> success_tasks{0};

  for (int32_t client_index{0}; client_index < kThreadCount; ++client_index) {
    /* 创建若干client线程 */
    clients.emplace_back([&, client_index] {
      const auto write_fd{in_pipe[client_index][1]};
      const auto read_fd{out_pipe[client_index][0]};

      for (int32_t task{0}; task < kTasksPerThread; ++task) {
        if (fail->failed_.load())
          /* 只要出错就立即返回 */
          return;

        /* 投递io任务 */
        if (!write_all(write_fd, payload.data(), payload.size())) {
          fail->set("client write error: " +
                    std::error_code(errno, std::system_category()).message());
          return;
        }

        /* 读取结果 */
        int32_t result{0};
        if (!read_all(read_fd, &result, sizeof(result))) {
          fail->set("client read error: " +
                    std::error_code(errno, std::system_category()).message());
          return;
        }

        /* 验证结果 */
        if (result == expected_count) {
          ++success_tasks;
        } else {
          fail->set("mismatch: client " + std::to_string(client_index) +
                    " task " + std::to_string(task) + " expected " +
                    std::to_string(expected_count) + " got " +
                    std::to_string(result));
          return;
        }
      }
    });
  }

  for (auto &t : clients) {
    if (t.joinable())
      t.join();
  }

  for (int32_t i{0}; i < kThreadCount; ++i) {
    close(in_pipe[i][1]);
    close(out_pipe[i][0]);
  }

  if (fail->failed_.load()) {
    FAIL() << fail->msg_;
  }

  ASSERT_EQ(success_tasks.load(), kThreadCount * kTasksPerThread);
}

/*-------------------- 测试生命周期管理 --------------------*/
TEST_F(KZCoreTest, LifeCheckerKeepsAliveDuringCallback) {
  /* 同步回调进度：已进入回调(entered)，顶层类释放点(release)，已结束回调(finished)
   */
  struct Sync {
    std::mutex mtx_{};
    std::condition_variable cv_{};
    bool entered_{false};
    bool release_{false};
    bool finished_{false};
  };
  auto sync = std::make_shared<Sync>();

  /* 创建单pipe */
  std::array<int32_t, 2> in_pipe{};
  ASSERT_EQ(pipe(in_pipe.data()), 0)
      << std::error_code(errno, std::system_category()).message();
  const auto read_fd{in_pipe[0]};
  const auto write_fd{in_pipe[1]};

  /* 监听的fd设置非阻塞 */
  set_nonblocking(read_fd);

  /* 被测试对象 */
  struct SessionTestLife
      : public std::enable_shared_from_this<SessionTestLife> {
    SessionTestLife(Loop &loop, const int32_t fd, std::shared_ptr<Sync> sync)
        : ch_{loop, fd}, sync_{std::move(sync)} {}

    void start() {
      ch_.update_event(
          weak_from_this(), EventType::kRead, EventMode::kDefault,
          [this](EventType) {
            /* 读取一个字节 */
            char tmp{};
            while (true) {
              const auto n = read(ch_.get_fd(), &tmp, sizeof(tmp));
              if (n > 0)
                continue;
              if (n < 0 && errno == EINTR)
                continue;
              if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
              // n == 0(对端关闭) 或 其他错误：这里直接 break 即可
              break;
            }
            /* 进入同步点->已进入 */
            {
              std::lock_guard lock{sync_->mtx_};
              sync_->entered_ = true;
            }
            sync_->cv_.notify_one();

            /* 进入同步点->顶层类已释放 */
            {
              std::unique_lock lock{sync_->mtx_};
              sync_->cv_.wait(lock, [&] { return sync_->release_; });
            }

            /* 测试保活 */
            this->touch();

            /* 进入同步点->已结束 */
            {
              std::lock_guard lock{sync_->mtx_};
              sync_->finished_ = true;
            }
            sync_->cv_.notify_one();
          });
    }

    /* 测试保活，使用session的内存区域，如果已析构->UAF报错+ASAN报错 */
    void touch() { data_ ^= 0x5A5A5A5A; }

    LoopChannel ch_;
    std::shared_ptr<Sync> sync_{};
    uint32_t data_{0x12345678};
  };

  /* 启动session */
  auto session = std::make_shared<SessionTestLife>(*loop_, read_fd, sync);
  const std::weak_ptr<SessionTestLife> session_checker{session};
  session->start();

  /* 投递一个任务 */
  constexpr char c{'x'};
  ASSERT_TRUE(write_all(write_fd, &c, 1))
      << std::error_code(errno, std::system_category()).message();

  /* 等待进入同步点->已进入 */
  {
    std::unique_lock lock{sync->mtx_};
    /* 确保已进入（2s没进入即失败） */
    ASSERT_TRUE(sync->cv_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return sync->entered_; }));
  }

  /* 提前释放session->由lambda保活 */
  session.reset();

  /* 确保成功释放session */
  const auto expired_before_release = session_checker.expired();

  EXPECT_FALSE(expired_before_release)
      << "session must still alive even release, keep alive failed.";

  /* 进入同步点->顶层类已释放 */
  {
    std::lock_guard lock{sync->mtx_};
    sync->release_ = true;
  }

  /* 接着执行回调->测试保活 */
  sync->cv_.notify_one();

  /* 进入同步点->已结束 */
  {
    std::unique_lock lock{sync->mtx_};
    /* 确保已结束（2s没结束即失败）-> 结束即为保活测试成功 */
    ASSERT_TRUE(sync->cv_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return sync->finished_; }));
  }

  /* 关闭资源 */
  close(write_fd);
  // read_fd 将由 SessionTestLife 析构 -> LoopChannel 析构 -> unregister_fd 关闭

  for (int32_t i{0}; i < 200 && !session_checker.expired(); ++i) {
    /* 等待session析构  */
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(session_checker.expired())
      << "session still alive after callback finished.";
}

/*-------------------- 测试disable_event() --------------------*/
TEST_F(KZCoreTest, DisableEventStopsCallback) {
  /* 同步点：用于等待 disable_event() 在 io 线程真正执行完 */
  struct Sync {
    std::mutex mtx_{};
    std::condition_variable cv_{};
    bool disable_applied_{false};
  };
  auto sync = std::make_shared<Sync>();

  /* 创建单pipe */
  std::array<int32_t, 2> in_pipe{};
  ASSERT_EQ(::pipe(in_pipe.data()), 0)
      << std::error_code(errno, std::system_category()).message();
  const auto read_fd{in_pipe[0]};
  const auto write_fd{in_pipe[1]};

  /* 监听的fd设置非阻塞 */
  set_nonblocking(read_fd);

  /* 被测试对象 */
  struct SessionTestDisableEvent
      : public std::enable_shared_from_this<SessionTestDisableEvent> {
    SessionTestDisableEvent(Loop &loop, const int32_t fd,
                            std::shared_ptr<Sync> sync)
        : ch_{loop, fd}, sync_{std::move(sync)} {}

    void start() {
      ch_.update_event(weak_from_this(), EventType::kRead, EventMode::kDefault,
                       [this](EventType) {
                         /* 如果 disable_event 生效，这里不应该被执行 */
                         cb_count_.fetch_add(1, std::memory_order_relaxed);

                         /* 读取 */
                         char tmp[64]{};
                         while (true) {
                           const auto n = read(ch_.get_fd(), tmp, sizeof(tmp));
                           if (n > 0) {
                             continue;
                           }
                           if (n < 0 && errno == EINTR) {
                             continue;
                           }
                           /* n == -1 && EAGAIN 或 n == 0 等情况都可退出 */
                           break;
                         }
                       });
    }

    void disable_and_sync() {
      /* 先投递 disable */
      ch_.disable_event();

      /* 再投递一个 io 任务作为屏障：保证它执行时 disable 已处理完 */
      {
        std::lock_guard lock{sync_->mtx_};
        sync_->disable_applied_ = false;
      }

      ch_.post_io_task(weak_from_this(), [this] {
        std::lock_guard lock{sync_->mtx_};
        sync_->disable_applied_ = true;
        sync_->cv_.notify_one();
      });

      /* 等待屏障任务执行完成（2s 还没完成说明 io 线程没跑起来/卡住） */
      std::unique_lock lock{sync_->mtx_};
      ASSERT_TRUE(sync_->cv_.wait_for(lock, std::chrono::seconds(2),
                                      [&] { return sync_->disable_applied_; }));
    }

    LoopChannel ch_;
    std::shared_ptr<Sync> sync_{};
    std::atomic<int32_t> cb_count_{0};
  };

  auto session =
      std::make_shared<SessionTestDisableEvent>(*loop_, read_fd, sync);
  session->start();

  /* 立刻 disable，并确保 disable 已在 io 线程生效 */
  session->disable_and_sync();

  /* 写入触发可读事件：如果 disable 正确，回调不应被调用 */
  constexpr char c{'x'};
  ASSERT_TRUE(write_all(write_fd, &c, 1))
      << std::error_code(errno, std::system_category()).message();

  /* 给 io 线程一点时间（即使有残留事件也不应回调） */
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_EQ(session->cb_count_.load(std::memory_order_relaxed), 0)
      << "callback executed after disable_event()";

  /* 关闭资源 */
  close(write_fd);
  // read_fd 将由 SessionTestDisableEvent 析构 -> LoopChannel 析构 ->
  // unregister_fd 关闭
  session.reset();
}

/*-------------------- 测试stop() 后可以再次 start() --------------------*/
TEST_F(KZCoreTest, StopThenStartWorks) {
  /* 同步点：用于等待回调执行到达指定次数 */
  struct Sync {
    std::mutex mtx_;
    std::condition_variable cv_{};
    int32_t handled_{0};
  };
  auto sync = std::make_shared<Sync>();

  /* 创建单pipe */
  std::array<int32_t, 2> in_pipe{};
  ASSERT_EQ(::pipe(in_pipe.data()), 0)
      << std::error_code(errno, std::system_category()).message();
  const auto read_fd{in_pipe[0]};
  const auto write_fd{in_pipe[1]};

  /* 监听的fd设置非阻塞 */
  set_nonblocking(read_fd);

  /* 被测试对象 */
  struct SessionTestRestart
      : public std::enable_shared_from_this<SessionTestRestart> {
    SessionTestRestart(Loop &loop, const int32_t fd, std::shared_ptr<Sync> sync)
        : ch_{loop, fd}, sync_{std::move(sync)} {}

    void start() {
      ch_.update_event(weak_from_this(), EventType::kRead, EventMode::kDefault,
                       [this](EventType) { on_read(); });
    }

    void on_read() const {
      /* ET读取：读到 EAGAIN 为止 */
      bool got_any{false};
      while (true) {
        char tmp[256]{};
        const auto n = read(ch_.get_fd(), tmp, sizeof(tmp));
        if (n > 0) {
          got_any = true;
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        /* n == -1 && EAGAIN 或 n == 0 等情况都可退出 */
        break;
      }

      if (got_any) {
        std::lock_guard lock{sync_->mtx_};
        sync_->handled_ += 1;
        sync_->cv_.notify_one();
      }
    }

    LoopChannel ch_;
    std::shared_ptr<Sync> sync_{};
  };

  auto session = std::make_shared<SessionTestRestart>(*loop_, read_fd, sync);
  session->start();

  /* 第一次触发：应当能回调一次 */
  {
    constexpr char c{'a'};
    ASSERT_TRUE(write_all(write_fd, &c, 1))
        << std::error_code(errno, std::system_category()).message();

    std::unique_lock lock{sync->mtx_};
    ASSERT_TRUE(sync->cv_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return sync->handled_ >= 1; }));
  }

  /* stop 后：再写入不应被处理（因为 executor 已停止） */
  loop_->stop();

  constexpr char c{'b'};
  ASSERT_TRUE(write_all(write_fd, &c, 1))
      << std::error_code(errno, std::system_category()).message();

  {
    std::lock_guard lock{sync->mtx_};
    EXPECT_EQ(sync->handled_, 1) << "callback executed while loop stopped.";
  }

  /* 再次 start：之前 stop 期间积累在 pipe 里的数据应被处理，回调次数变为 2 */
  loop_->start();
  {
    std::unique_lock lock{sync->mtx_};
    ASSERT_TRUE(sync->cv_.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return sync->handled_ >= 2; }));
  }

  /* 关闭资源 */
  close(write_fd);
  session.reset();
  /* read_fd 将由 SessionTestRestart 析构 -> LoopChannel 析构 -> unregister_fd
   * 关闭 */
}
