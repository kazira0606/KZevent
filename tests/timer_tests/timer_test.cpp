#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kzevent/core.hpp"
#include "kzevent/timer/timer.hpp"

/* 测试类 */
class KZTimerTest : public testing::Test {
protected:
  void SetUp() override {
    /* 启动 Loop */
    loop_ = std::make_unique<kzevent::core::Loop>();

    /* 创建 Timer */
    timer_ = kzevent::timer::Timer::make_timer(*loop_);

    loop_->start();
  }

  void TearDown() override {
    /* 停止并释放资源 */
    timer_.reset();
    loop_->stop();
    loop_.reset();
  }

  std::unique_ptr<kzevent::core::Loop> loop_{};
  std::shared_ptr<kzevent::timer::Timer> timer_{};
};

/*-------------------- 单次任务测试 --------------------*/
TEST_F(KZTimerTest, PostOnceTaskRandomTest) {
  /* 基础字符串 */
  const std::string base_str{"Hello Timer"};

  /* 统计成功完成的任务数 */
  std::atomic<int32_t> success_count{0};

  /* 线程容器 */
  std::vector<std::thread> threads{};
  threads.reserve(KZEVENT_TIMER_CLIENT_NUM);

  for (size_t i{0}; i < KZEVENT_TIMER_CLIENT_NUM; ++i) {
    threads.emplace_back([this, &success_count, base_str, i]() {
      /* 每个线程独立的随机数生成器 */
      std::random_device rd;
      std::mt19937 gen(rd());
      /* 随机超时时间 50ms ~ 250ms */
      std::uniform_int_distribution<uint64_t> dist(50, 250);
      const uint64_t timeout_ms = dist(gen);

      /* 构造预期结果 */
      const std::string expected_str = base_str + "_" + std::to_string(i);

      /* 模拟 Owner 生命周期 */
      auto owner = std::make_shared<bool>(true);

      /* 使用 promise 同步结果 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 记录提交时间 */
      const auto start_time = std::chrono::steady_clock::now();

      /* 提交任务 */
      timer_->post_once_task(
          [done, start_time, i, base_str, expected_str, timeout_ms,
           &success_count]() {
            /* 1. 验证字符串处理逻辑 */
            std::string result = base_str + "_" + std::to_string(i);
            EXPECT_EQ(result, expected_str);

            /* 2. 验证定时器精度 */
            const auto end_time = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time)
                    .count();

            /* 实际耗时必须 >= 设定时间 */
            EXPECT_GE(elapsed, timeout_ms);

            /* 允许一定的调度延迟误差，测试代码误差定为 5ms */
            EXPECT_LE(elapsed, timeout_ms + 5)
                << "timer task " << i
                << " delayed too much. expected: " << timeout_ms
                << "ms, actual: " << elapsed << "ms";

            success_count.fetch_add(1);
            done->set_value();
          },
          owner, timeout_ms);

      /* 等待任务完成 (给予比超时时间稍长的等待窗口) */
      if (future.wait_for(std::chrono::milliseconds(timeout_ms + 1000)) !=
          std::future_status::ready) {
        ADD_FAILURE() << "task " << i << " timed out waiting for execution.";
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), KZEVENT_TIMER_CLIENT_NUM);
}

/*-------------------- 循环任务测试 --------------------*/
TEST_F(KZTimerTest, PostRepeatTaskRandomTest) {
  std::atomic<int32_t> success_count{0};
  std::vector<std::thread> threads{};
  threads.reserve(KZEVENT_TIMER_CLIENT_NUM);

  for (size_t i{0}; i < KZEVENT_TIMER_CLIENT_NUM; ++i) {
    threads.emplace_back([this, &success_count, i]() {
      std::random_device rd;
      std::mt19937 gen(rd());

      /* 随机间隔 20ms ~ 50ms */
      std::uniform_int_distribution<uint64_t> dist_interval(20, 50);
      const uint64_t interval_ms = dist_interval(gen);

      /* 随机运行次数 3 ~ 6 次 */
      std::uniform_int_distribution<int32_t> dist_count(3, 6);
      const int32_t target_runs = dist_count(gen);

      auto owner = std::make_shared<bool>(true);
      auto done = std::make_shared<std::promise<void>>();
      auto future = done->get_future();

      /* 上下文数据，用于在多次回调间保持状态 */
      struct Context {
        int32_t run_count = 0;
        std::chrono::steady_clock::time_point last_time;
      };
      auto ctx = std::make_shared<Context>();
      ctx->last_time = std::chrono::steady_clock::now();

      /* 提交循环任务 */
      const auto timer_id = timer_->post_repeat_task(
          [done, ctx, interval_ms, target_runs, &success_count, i]() {
            ctx->run_count++;

            /* 验证每次调用的间隔 */
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - ctx->last_time)
                    .count();
            ctx->last_time = now;

            /* 允许 -5ms (超时唤醒) 到 +5ms (延迟) 的误差 */
            if (ctx->run_count > 1) {
              EXPECT_GE(elapsed + 5, interval_ms);
              EXPECT_LE(elapsed, interval_ms + 5)
                  << "thread " << i << " run " << ctx->run_count
                  << " interval drift.";
            }

            /* 达到目标次数，通知测试结束 */
            if (ctx->run_count >= target_runs) {
              success_count.fetch_add(1);
              done->set_value();
            }
          },
          owner, interval_ms);

      /* 等待足够的总时长 */
      const auto wait_total = interval_ms * target_runs + 1000;
      if (future.wait_for(std::chrono::milliseconds(wait_total)) !=
          std::future_status::ready) {
        ADD_FAILURE() << "repeat task " << i << " timed out.";
      }

      /* 取消任务，防止后台空转 */
      timer_->cancel_task(timer_id);
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(success_count.load(), KZEVENT_TIMER_CLIENT_NUM);
}

/*-------------------- 取消任务测试 --------------------*/
TEST_F(KZTimerTest, CancelTaskTest) {
  std::atomic<int32_t> fail_count{0};
  std::vector<std::thread> threads{};
  threads.reserve(KZEVENT_TIMER_CLIENT_NUM);

  for (size_t i{0}; i < KZEVENT_TIMER_CLIENT_NUM; ++i) {
    threads.emplace_back([this, &fail_count]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      /* 较长的超时时间，确保有时间取消 */
      std::uniform_int_distribution<uint64_t> dist(200, 500);
      const uint64_t timeout_ms = dist(gen);

      auto owner = std::make_shared<bool>(true);

      /* 提交一个任务，如果它被执行了，测试失败 */
      const auto id = timer_->post_once_task(
          [&fail_count]() {
            fail_count.fetch_add(1);
            ADD_FAILURE() << "cancelled task was incorrectly executed!";
          },
          owner, timeout_ms);

      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms / 10));

      timer_->cancel_task(id);

      /* 等待超过任务原本应该触发的时间，确保取消 */
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms + 50));
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  /* 验证失败计数为0 */
  EXPECT_EQ(fail_count.load(), 0);
}