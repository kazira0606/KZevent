#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

namespace kzevent::log {
enum class Level : std::uint8_t {
  kInfo,
  kError,
  kFatal,
};
} // namespace kzevent::log

namespace kzevent::log::detail {
void log_impl(Level level, std::string &&payload) noexcept;

template <typename... Args> void log(const Level level, Args &&...args) {
  std::stringstream output;
  (output << ... << std::forward<Args>(args));
  log_impl(level, output.str());
}
} // namespace kzevent::log::detail

#define KZ_LOG_INFO(...)                                                       \
  do {                                                                         \
    kzevent::log::detail::log(kzevent::log::Level::kInfo, "[", __FILE__, ":",  \
                              __LINE__, "] ", ##__VA_ARGS__);                  \
  } while (false)

#define KZ_LOG_ERROR(...)                                                      \
  do {                                                                         \
    kzevent::log::detail::log(kzevent::log::Level::kError, "[", __FILE__, ":", \
                              __LINE__, "] ", ##__VA_ARGS__);                  \
  } while (false)

#define KZ_LOG_FATAL(...)                                                      \
  do {                                                                         \
    kzevent::log::detail::log(kzevent::log::Level::kFatal, "[", __FILE__, ":", \
                              __LINE__, "] ", ##__VA_ARGS__);                  \
  } while (false)
