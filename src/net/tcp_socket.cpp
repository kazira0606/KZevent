#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <arpa/inet.h>
#include <string_view>
#include <sys/types.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/tcp_socket.hpp"

namespace kzevent::net::tcp {
TcpClient::~TcpClient() { stop(); }

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

  const std::string_view stream_slice{
      reinterpret_cast<char *>(recv_buf_.begin()),
      static_cast<size_t>(recv_buf_.size())};

  return split_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()),
                   stream_slice);
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

void TcpClient::on_error(int32_t err) {
  KZ_LOG_ERROR("tcp client err: ", std::system_category().message(err));

  if (error_cb_ == nullptr) {
    /* 未注册错误断连回调 */
    return;
  }

  switch (err) {
  case ECONNREFUSED:
  case ETIMEDOUT:
  case ENETUNREACH:
  case EHOSTUNREACH:
  case EADDRNOTAVAIL:
  case ECONNRESET: {
    if (error_cb_ != nullptr) {
      error_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()));
    }
    break;
  }

  default:
    break;
  }
}

void TcpClient::on_disconnect() {
  if (disconnect_cb_ == nullptr) {
    /* 未注册正常断连回调 -> 不处理 */
    return;
  }
  disconnect_cb_(std::static_pointer_cast<TcpClient>(shared_from_this()));
}
} // namespace kzevent::net::tcp