#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <thread>
#include <filesystem>
#include <ctime>

#include "kzevent/log.hpp"

namespace fs = std::filesystem;

namespace kzevent::log::detail {
  class LogManager {
  public:
    static LogManager &instance() {
      static LogManager manager;
      return manager;
    }

    void write(const Level level, const std::string &payload) {
      std::lock_guard write_lock(write_mtx_);

      /* 检查是否需要启用新文件 */
      if (!log_file_.is_open() || current_line_count_ >= kMaxLines) {
        new_log_file();
      }

      /* 准备C风格时间戳 */
      const auto now = std::chrono::system_clock::now();
      const auto in_time_t = std::chrono::system_clock::to_time_t(now);

      /* 准备time结构体(Linux线程安全版本) */
      std::tm tm_buf{};
      localtime_r(&in_time_t, &tm_buf);

      /* 写入日志 */
      log_file_ << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "] "
          << "[" << level_to_string(level) << "] "
          << "[TID:" << std::this_thread::get_id() << "] "
          << payload << std::endl;

      /* 统计行数 */
      current_line_count_ += 1;
    }

  private:
    LogManager() {
      /* 使用CMake配置的宏 */
      log_dir_ = fs::path(KZ_EVENT_LOG_DIR) / "kz_logs";

      /* 目录不存在->创建目录 */
      if (std::error_code ec; !fs::exists(log_dir_)) {
        fs::create_directories(log_dir_, ec);
        if (ec) {
          std::cerr << "failed to create kz_logs directory: " << log_dir_ << ", error: " << ec.message() << std::endl;
        }
      }
    }

    ~LogManager() {
      if (log_file_.is_open()) {
        log_file_.close();
      }
    }

    void new_log_file() {
      if (log_file_.is_open()) {
        log_file_.close();
      }

      const auto now = std::chrono::system_clock::now();
      const auto in_time_t = std::chrono::system_clock::to_time_t(now);
      std::tm tm_buf{};
      localtime_r(&in_time_t, &tm_buf);

      std::stringstream file_name;
      /* 拼接完整路径例: /home/user/project/kz_logs/kz_log_xxxx-xx-xx_xx-xx-xx.txt */
      file_name << "kz_log_"
          << std::put_time(&tm_buf, "%Y-%m-%d_%H-%M-%S")
          << ".txt";

      const fs::path file_path = log_dir_ / file_name.str();

      /* 文件名重复的时候使用append模式 */
      log_file_.open(file_path, std::ios::out | std::ios::app);

      if (!log_file_.is_open()) {
        std::cerr << "FATAL: failed to open kz_logs file: " << file_path << std::endl;
      }

      current_line_count_ = 0;
    }

    static const char *level_to_string(const Level level) {
      switch (level) {
        case Level::kInfo: return "INFO ";
        case Level::kError: return "ERROR";
        case Level::kFatal: return "FATAL";
        default: return "UNKNO";
      }
    }

    static constexpr int kMaxLines = 5000;

    std::mutex write_mtx_;
    std::ofstream log_file_;
    fs::path log_dir_;
    int current_line_count_ = 0;
  };

  void log_impl(const Level level, std::string &&payload) noexcept {
    try {
      LogManager::instance().write(level, payload);
    } catch (...) {
      /* 吞掉异常，该日志写入失败 */
    }
  }
} // namespace::kzevent::log::detail
