#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <arpa/inet.h>
#include <sys/types.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/tcp_socket.hpp"

namespace kzevent::net::tcp {
std::shared_ptr<TcpClient> TcpClient::make_tcp_client(core::Loop &loop,
                                                      const InetAddr &local) {
  if (local.get_sockaddr()->sa_family != AF_INET &&
      local.get_sockaddr()->sa_family != AF_INET6) {
    /* tcp socket 只支持 ipv4 和 ipv6 */
    KZ_LOG_ERROR("tcp client err: unsupported protocol");
    return {};
  }

  struct EnableMakeShared : public TcpClient {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : TcpClient(loop, local) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop, local);
  return ret;
}

void TcpClient::set_split_cb(SplitCallBack cb) noexcept {
  split_cb_ = std::move(cb);
}

void TcpClient::set_fragment_cb(FragmentCallBack cb) noexcept {
  fragment_cb_ = std::move(cb);
}

void TcpClient::set_error_cb(DisconnectCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

void TcpClient::set_disconnect_cb(DisconnectCallBack cb) noexcept {
  disconnect_cb_ = std::move(cb);
}

TcpClient::TcpClient(core::Loop &loop, const InetAddr &local)
    : StreamClientSocket(loop, local) {}

ssize_t TcpClient::on_split() {
  if (split_cb_ == nullptr) {
    /* 未注册切包回调 -> 不切分 */
    return 0;
  }

  const std::string_view slice{reinterpret_cast<char *>(recv_buf_.begin()),
                               static_cast<size_t>(recv_buf_.size())};

  return split_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()),
                   slice);
}

void TcpClient::on_fragment(std::vector<uint8_t> fragment) {
  if (fragment_cb_ == nullptr) {
    /* 未注册执行回调 -> 不执行 */
    return;
  }

  auto task = [this, fragment = std::move(fragment)]() mutable {
    fragment_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()),
                 std::move(fragment));
  };

  post_heavy_task(std::move(task));
}

void TcpClient::on_error(const int32_t err) {
  KZ_LOG_ERROR("tcp client err: ", std::system_category().message(err));
  stop();

  if (error_cb_ == nullptr) {
    /* 未注册错误断连回调 -> 不处理 */
    return;
  }

  switch (err) {
  case ECONNREFUSED:
  case ETIMEDOUT:
  case ENETUNREACH:
  case EHOSTUNREACH:
  case EADDRNOTAVAIL: {
    error_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()));
    break;
  }

  default:
    break;
  }
}

void TcpClient::on_disconnect() {
  stop();

  if (disconnect_cb_ == nullptr) {
    /* 未注册正常断连回调 -> 不处理 */
    return;
  }
  disconnect_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()));
}

std::shared_ptr<TcpServer> TcpServer::make_tcp_server(core::Loop &loop,
                                                      const InetAddr &local) {
  if (local.get_sockaddr()->sa_family != AF_INET &&
      local.get_sockaddr()->sa_family != AF_INET6) {
    /* tcp socket 只支持 ipv4 和 ipv6 */
    KZ_LOG_ERROR("tcp server err: unsupported protocol");
    return {};
  }

  struct EnableMakeShared : public TcpServer {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : TcpServer(loop, local) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop, local);
  ret->start();
  return ret;
}

void TcpServer::set_split_cb(SplitCallBack cb) noexcept {
  split_cb_ = std::move(cb);
}

void TcpServer::set_fragment_cb(FragmentCallBack cb) noexcept {
  fragment_cb_ = std::move(cb);
}

void TcpServer::set_session_error_cb(DisconnectCallBack cb) noexcept {
  session_error_cb_ = std::move(cb);
}

void TcpServer::set_disconnect_cb(DisconnectCallBack cb) noexcept {
  disconnect_cb_ = std::move(cb);
}

TcpServer::TcpServer(core::Loop &loop, const InetAddr &local)
    : StreamServerSocket(loop, local) {}

ssize_t TcpServer::on_split(const Session session) {
  if (split_cb_ == nullptr) {
    /* 未注册切包回调 -> 不切分 */
    return 0;
  }

  const std::string_view slice{
      reinterpret_cast<char *>(session->recv_buf_.begin()),
      static_cast<size_t>(session->recv_buf_.size())};

  return split_cb_(session, slice);
}

void TcpServer::on_fragment(std::vector<uint8_t> fragment, Session session) {
  if (fragment_cb_ == nullptr) {
    /* 未注册执行回调 -> 不执行 */
    return;
  }

  auto task = [this, fragment = std::move(fragment), session]() mutable {
    fragment_cb_(session, std::move(fragment));
  };

  session->post_heavy_task(std::move(task));
}

void TcpServer::on_session_error(const int32_t err, const Session session) {
  KZ_LOG_ERROR("tcp client err: ", std::system_category().message(err));
  session->stop();

  if (session_error_cb_ == nullptr) {
    /* 未注册错误断连回调 -> 不处理 */
    return;
  }

  switch (err) {
  case ECONNREFUSED:
  case ETIMEDOUT:
  case ENETUNREACH:
  case EHOSTUNREACH:
  case EADDRNOTAVAIL: {
    session_error_cb_(session);
    break;
  }

  default:
    break;
  }
}

void TcpServer::on_disconnect(const Session session) {
  session->stop();

  if (disconnect_cb_ == nullptr) {
    /* 未注册正常断连回调 -> 不处理 */
    return;
  }
  disconnect_cb_(session);
}
} // namespace kzevent::net::tcp