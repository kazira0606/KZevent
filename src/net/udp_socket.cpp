#include <cassert>
#include <cstdint>
#include <memory>
#include <system_error>
#include <utility>

#include <arpa/inet.h>
#include <unistd.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

namespace kzevent::net::udp {
/*-------------------- UDP node  --------------------*/
std::shared_ptr<UdpNode> UdpNode::make_udp_node(core::Loop &loop,
                                                const InetAddr &local) {
  if (local.get_sockaddr()->sa_family != AF_INET &&
      local.get_sockaddr()->sa_family != AF_INET6) {
    /* udp socket 只支持 ipv4 和 ipv6 */
    KZ_LOG_ERROR("udp node err: unsupported protocol");
    return {};
  }

  struct EnableMakeShared : public UdpNode {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : UdpNode(loop, local) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop, local);
  ret->start();
  return ret;
}

void UdpNode::set_read_cb(NodeCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

UdpNode::UdpNode(core::Loop &loop, const InetAddr &local)
    : DgramSocket(loop, local) {}

void UdpNode::on_read(std::vector<uint8_t> data, const InetAddr &source) {
  if (read_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }

  /* 强制数据处理在heavy线程 */
  auto task = [this, data = std::move(data), source]() mutable {
    read_cb_(std::static_pointer_cast<UdpNode>(shared_from_this()),
             std::move(data), source);
  };

  post_heavy_task(std::move(task));
}

void UdpNode::on_error(const int32_t err) {
  KZ_LOG_ERROR("udp node err: ", std::system_category().message(err));
}

/*-------------------- UDP client  --------------------*/
std::shared_ptr<UdpClient> UdpClient::make_udp_client(core::Loop &loop,
                                                      const InetAddr &local,
                                                      const InetAddr &source) {
  if (local.get_sockaddr()->sa_family != AF_INET &&
      local.get_sockaddr()->sa_family != AF_INET6) {
    /* udp socket 只支持 ipv4 和 ipv6 */
    KZ_LOG_ERROR("udp client err: unsupported protocol");
    return {};
  }

  if (local.get_sockaddr()->sa_family != source.get_sockaddr()->sa_family) {
    /* 源地址和目标地址的协议不一致 */
    KZ_LOG_ERROR("udp client err: source and target protocol mismatch");
    return {};
  }

  struct EnableMakeShared : public UdpClient {
    EnableMakeShared(core::Loop &loop, const InetAddr &local,
                     const InetAddr &source)
        : UdpClient(loop, local, source) {}
  };
  auto ret = std::make_shared<EnableMakeShared>(loop, local, source);
  ret->start();
  return ret;
}

UdpClient::UdpClient(core::Loop &loop, const InetAddr &local,
                     const InetAddr &source)
    : DgramSocket(loop, local), source_(source) {}

void UdpClient::set_read_cb(ClientCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

void UdpClient::set_error_cb(ErrorCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

void UdpClient::on_read(std::vector<uint8_t> data, const InetAddr &source) {
  if (read_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }

  if (source != source_) {
    KZ_LOG_INFO("udp client read data from unexpected source");
    return;
  }

  /* 强制数据处理在heavy线程 */
  auto task = [this, data = std::move(data)]() mutable {
    read_cb_(std::static_pointer_cast<UdpClient>(shared_from_this()),
             std::move(data));
  };

  post_heavy_task(std::move(task));
}

void UdpClient::on_error(const int32_t err) {
  KZ_LOG_ERROR("udp client err: ", std::system_category().message(err));

  if (error_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }

  if (err == ECONNREFUSED) {
    /* 无效的发送地址 */
    error_cb_();
  }
}

/*-------------------- UDP session  --------------------*/
std::shared_ptr<UdpServer::UdpSession> UdpServer::UdpSession::make_udp_session(
    const std::shared_ptr<UdpServer> &server, const InetAddr &source,
    std::chrono::steady_clock::time_point time_stamp) {

  if (server->local_.get_sockaddr()->sa_family !=
      source.get_sockaddr()->sa_family) {
    /* 源地址和目标地址的协议不一致 */
    KZ_LOG_ERROR("udp session err: source and target protocol mismatch");
  }

  struct EnableMakeShared : public UdpServer::UdpSession {
    EnableMakeShared(const std::shared_ptr<UdpServer> &server,
                     const InetAddr &source,
                     const std::chrono::steady_clock::time_point time_stamp)
        : UdpServer::UdpSession(server, source, time_stamp) {}
  };
  return std::make_shared<EnableMakeShared>(server, source, time_stamp);
}

void UdpServer::UdpSession::set_user_context(
    std::shared_ptr<void> user_context) noexcept {
  user_context_ = std::move(user_context);
}

[[nodiscard]] std::shared_ptr<void>
UdpServer::UdpSession::get_user_context() const noexcept {
  return user_context_;
}

UdpServer::UdpSession::UdpSession(
    const std::shared_ptr<UdpServer> &server, const InetAddr &source,
    const std::chrono::steady_clock::time_point time_stamp)
    : source_(source), time_stamp_(time_stamp), server_(server) {}

/*-------------------- UDP server  --------------------*/
std::shared_ptr<UdpServer>
UdpServer::make_udp_server(core::Loop &loop, const InetAddr &local,
                           uint64_t session_timeout_ms) {
  if (local.get_sockaddr()->sa_family != AF_INET &&
      local.get_sockaddr()->sa_family != AF_INET6) {
    /* udp socket 只支持 ipv4 和 ipv6 */
    KZ_LOG_ERROR("udp server err: unsupported protocol");
    return {};
  }

  if (session_timeout_ms == 0) {
    /* 超时时长不能为 0 */
    KZ_LOG_ERROR("udp server err: session_timeout_ms cannot be 0");
    return {};
  }

  struct EnableMakeShared : public UdpServer {
    EnableMakeShared(core::Loop &loop, const InetAddr &local,
                     const uint64_t session_timeout_ms)
        : UdpServer(loop, local, session_timeout_ms) {}
  };
  auto ret =
      std::make_shared<EnableMakeShared>(loop, local, session_timeout_ms);
  ret->start();
  return ret;
}

void UdpServer::start() {
  /* 基类启动 */
  DgramSocket::start();

  if (bool expected{false}; !started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto task = [this](const core::EventType) {
    uint64_t val;
    while (read(timer_channel_.get_fd(), &val, sizeof(val)) > 0)
      ;
    if (errno != EAGAIN) {
      sys_error::fatal();
    }

    const auto now = std::chrono::steady_clock::now();
    auto it = sessions_.begin();
    while (it != sessions_.end()) {
      /* 扫描所有会话距离上一次接收数据的时间间隔 */
      if (const auto duration = now - it->second->time_stamp_;
          duration > std::chrono::milliseconds{session_timeout_ms_}) {
        /* 会话超时 */
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
  };

  timer_channel_.update_event(weak_from_this(), core::EventType::kRead,
                              core::EventMode::kDefault, std::move(task));
}

void UdpServer::stop() {
  /* 基类停止 */
  DgramSocket::stop();

  started_ = false;
  timer_channel_.disable_event();
}

void UdpServer::set_new_session_cb(ServerCallBack cb) noexcept {
  new_session_cb_ = std::move(cb);
}

void UdpServer::set_read_cb(ServerCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

UdpServer::UdpServer(core::Loop &loop, const InetAddr &local,
                     uint64_t session_timeout_ms)
    : DgramSocket(loop, local), local_(local),
      timer_channel_([&loop, session_timeout_ms]() -> core::LoopChannel {
        /* 默认策略，10倍频率扫描->误差10% */
        auto scan_time = session_timeout_ms / kScanFrequencyDivisor;

        if (scan_time < kMinScanIntervalMs) {
          /* 最高扫描频率->100ms扫描一次降低cpu负载 */
          scan_time = kMinScanIntervalMs;
        } else if (scan_time > kMaxScanIntervalMs) {
          /* 最低扫描频率->10000ms扫描一次保证内存占用 */
          scan_time = kMaxScanIntervalMs;
        }

        auto ch = make_timer_channel(loop, scan_time, scan_time);

        if (!ch.has_value()) {
          throw std::runtime_error("make timer channel failed");
        }

        return std::move(ch).value();
      }()),
      session_timeout_ms_(session_timeout_ms) {}

void UdpServer::on_read(std::vector<uint8_t> data, const InetAddr &source) {
  if (read_cb_ == nullptr && new_session_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }

  /* 是否是已有会话 */
  const auto it = sessions_.find(source);

  if (it == sessions_.end()) {
    /* 新会话 */
    const auto new_session = UdpSession::make_udp_session(
        std::static_pointer_cast<UdpServer>(shared_from_this()), source,
        std::chrono::steady_clock::now());

    /* 加入管理 */
    sessions_.emplace(source, new_session);

    if (new_session_cb_ != nullptr) {
      /* 处理新连接回调 */
      /* 强制数据处理在heavy线程 */
      auto task = [this, new_session, data = std::move(data)]() mutable {
        new_session_cb_(new_session, std::move(data));
      };

      post_heavy_task(std::move(task));

    } else {

      /* 强制数据处理在heavy线程 */
      auto task = [this, new_session, data = std::move(data)]() mutable {
        read_cb_(new_session, std::move(data));
      };

      post_heavy_task(std::move(task));
    }
    return;
  }

  /* 已有会话 */
  if (read_cb_ == nullptr) {
    return;
  }
  it->second->time_stamp_ = std::chrono::steady_clock::now();

  auto task = [this, session = it->second, data = std::move(data)]() mutable {
    read_cb_(session, std::move(data));
  };

  post_heavy_task(std::move(task));
}

void UdpServer::on_error(const int32_t err) {
  KZ_LOG_ERROR("udp server err: ", std::system_category().message(err));
}
} // namespace kzevent::net::udp