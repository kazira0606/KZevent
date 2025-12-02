#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "kzevent/log.hpp"

namespace fs = std::filesystem;

/* 工具函数->随机数生成 */
static int32_t random_int() {
  static std::mt19937 gen{std::random_device{}()};
  static std::uniform_int_distribution dis(-1000, 1000);
  return dis(gen);
}

/* 工具函数->获取最新日志 */
static std::optional<std::filesystem::path>
get_latest_file(const std::filesystem::path &dir) {
  fs::path latest_file{};
  fs::file_time_type latest_time{};

  for (const auto &entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".txt") {
      if (latest_file.empty() || entry.last_write_time() > latest_time) {
        latest_time = entry.last_write_time();
        latest_file = entry.path();
      }
    }
  }

  if (latest_file.empty()) {
    return std::nullopt;
  }
  return latest_file;
}

/* 工具函数->读取文件内容 */
static std::string read_file_content(const fs::path &path) {
  std::ifstream log_file(path);
  std::stringstream log_content;
  log_content << log_file.rdbuf();
  return log_content.str();
}

/* 测试类 */
class KZLogTest : public testing::Test {
protected:
  void SetUp() override {
    if (fs::exists(log_dir_)) {
      for (const auto &entry : fs::directory_iterator(log_dir_)) {
        fs::remove(entry.path());
      }
    }
  }

  fs::path log_dir_ = fs::path(KZ_EVENT_LOG_DIR) / "kz_logs";
};

TEST_F(KZLogTest, VerifyContentWithMultiThreads) {
  /* 随机字符串 */
  std::string token = "TOKEN_" + std::to_string(random_int());

  /* 并发量配置 */
  constexpr int32_t thread_count = 10;
  constexpr int32_t logs_per_thread = 50;

  /* 线程池和异常记录 */
  std::vector<std::thread> threads;
  std::atomic<int32_t> error_count{0};

  /* 并发测试 */
  threads.reserve(thread_count);
  for (int32_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    threads.emplace_back([token, thread_index, &error_count] {
      try {
        for (int32_t log_index = 0; log_index < logs_per_thread; ++log_index) {
          KZ_LOG_INFO(token, " ", "Thread-", thread_index, " ", token, " Msg-",
                      log_index, " ", token);
          KZ_LOG_ERROR(token, " ", "Thread-", thread_index, " ", token, " Msg-",
                       log_index, " ", token);
          KZ_LOG_FATAL(token, " ", "Thread-", thread_index, " ", token, " Msg-",
                       log_index, " ", token);
        }
      } catch (...) {
        ++error_count;
      }
    });
  }

  /* 线程结束 */
  for (auto &t : threads) {
    if (t.joinable())
      t.join();
  }

  /* 验证是否抛异常 */
  ASSERT_EQ(error_count, 0) << "Some threads threw exceptions!";

  /* 验证log文件创建是否成功 */
  const auto log_file = get_latest_file(log_dir_);
  ASSERT_TRUE(log_file.has_value());

  /* 读取文件内容 */
  std::stringstream content(read_file_content(log_file.value()));
  std::string one_line;
  int32_t valid_lines = 0;

  /* 验证每一行完整度 */
  while (std::getline(content, one_line)) {
    if (one_line.empty())
      continue;

    /* 每一行必须3token */
    int32_t token_hits = 0;
    size_t pos = 0;

    /* 循环查找 token 出现的次数 */
    while ((pos = one_line.find(token, pos)) != std::string::npos) {
      token_hits++;
      pos += token.length();
    }

    if (token_hits != 3) {
      /* 日志有破损行 */
      ADD_FAILURE() << "Corrupted line found! Expected 3 tokens, found "
                    << token_hits << "in " << "line: " << one_line;
      continue;
    }

    valid_lines++;
  }

  /* 验证日志完整度  */
  constexpr int32_t expect_lines = thread_count * logs_per_thread * 3;
  EXPECT_EQ(valid_lines, expect_lines)
      << "Expected " << expect_lines << " lines, but got " << valid_lines;
}
