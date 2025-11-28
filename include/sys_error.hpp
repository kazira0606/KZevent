#pragma once

#include <system_error>
#include <cerrno>

#include "log.hpp"

namespace kzevent::sys_error {
  /* error级 */
  inline void error(const std::exception& e) {
    KZ_LOG_ERROR(
      "msg: ", e.what()
    );
  }

  inline void error() {
    const auto saved_errno = errno;
    const std::error_code ec{saved_errno, std::system_category()};

    KZ_LOG_ERROR(
      "errno: ", saved_errno,
      ", msg: ", ec.message()
    );
  }

  /* fatal级 */
  [[noreturn]]
  inline void fatal(const std::exception& e) {
    KZ_LOG_FATAL(
      "msg: ", e.what()
    );
    throw;
  }

  [[noreturn]]
  inline void fatal() {
    const auto saved_errno = errno;
    const std::error_code ec{saved_errno, std::system_category()};

    KZ_LOG_FATAL(
      "errno: ", saved_errno,
      ", msg: ", ec.message()
    );

    throw std::system_error(ec);
  }
} // namespace kzevent::sys_error
