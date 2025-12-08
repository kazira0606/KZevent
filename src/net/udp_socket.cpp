#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include <arpa/inet.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

namespace kzevent::net::udp {
std::shared_ptr<UdpNode> UdpNode::make_by_any(core::Loop &loop,
                                              const UdpType type,
                                              const uint16_t port) {

  /* 间接让make_shared()访问到构造函数 */
  struct MakeSharedEnabler final : UdpNode {
    MakeSharedEnabler(core::Loop &loop, const InetAddr &addr)
        : UdpNode(loop, addr) {}
  };

  switch (type) {
  case UdpType::kUdpIpv4: {
    const auto addr = InetAddr::make_ipv4("0.0.0.0", port);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  case UdpType::kUdpIpv6: {
    const auto addr = InetAddr::make_ipv6("::", port);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  default:
    KZ_LOG_ERROR("invalid udp type");
    return {};
  }
}

std::shared_ptr<UdpNode> UdpNode::make_by_ip(core::Loop &loop,
                                             const UdpType type,
                                             const std::string &ip,
                                             const uint16_t port) {
  /* 间接让make_shared()访问到构造函数 */
  struct MakeSharedEnabler final : UdpNode {
    MakeSharedEnabler(core::Loop &loop, const InetAddr &addr)
        : UdpNode(loop, addr) {}
  };

  switch (type) {
  case UdpType::kUdpIpv4: {
    const auto addr = InetAddr::make_ipv4(ip, port);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  case UdpType::kUdpIpv6: {
    const auto addr = InetAddr::make_ipv6(ip, port);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  default:
    KZ_LOG_ERROR("invalid udp type");
    return {};
  }
}

std::shared_ptr<UdpNode> UdpNode::make_by_path(core::Loop &loop,
                                               const UdpType type,
                                               const std::string &path) {
  /* 间接让make_shared()访问到构造函数 */
  struct MakeSharedEnabler final : UdpNode {
    MakeSharedEnabler(core::Loop &loop, const InetAddr &addr)
        : UdpNode(loop, addr) {}
  };

  switch (type) {
  case UdpType::kUnix: {
    const auto addr = InetAddr::make_unix(path);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  case UdpType::kAbstract: {
    const auto addr = InetAddr::make_abstract(path);
    if (!addr.has_value()) {
      return {};
    }

    return std::make_shared<MakeSharedEnabler>(loop, addr.value());
  }

  default:
    KZ_LOG_ERROR("invalid udp type");
    return {};
  }
}

UdpNode::UdpNode(core::Loop &loop, const InetAddr &addr)
    : udp_channel_([&]() -> core::LoopChannel {
        auto ch = make_udp_channel(loop, addr);

        if (!ch.has_value()) {
          throw std::runtime_error("make udp channel failed");
        }

        return std::move(ch).value();
      }()),
      recv_buf_(65535, 0) {}

void UdpNode::set_recv_cb(ReadCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

void UdpNode::set_error_cb(ErrorCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

void UdpNode::start() {
  auto cb = [this](const core::EventType event_types) {
    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      /* 可读事件 */
      if (!read_cb_) {
        /* 还未注册回调 */
        ;
      } else {
        while (true) {
          recv_buf_.resize(65536);
          /* 循环读取所有的包 */
          const auto [ret, source_addr] = udp_recv(udp_channel_, recv_buf_);
          if (!source_addr.has_value()) {
            /* EAGAIN或系统错误(有log) */
            break;
          }
          recv_buf_.resize(ret);
          read_cb_(shared_from_this(), recv_buf_, source_addr.value());
        }
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件 */
      if (!error_cb_) {
        /* 还未注册回调 */
        ;
      } else {
        error_cb_();
      }
    }
  };

  udp_channel_.update_event(weak_from_this(),
                            core::EventType::kRead | core::EventType::kError,
                            core::EventMode::kDefault, std::move(cb));
}

void UdpNode::stop() { udp_channel_.disable_event(); }
} // namespace kzevent::net::udp