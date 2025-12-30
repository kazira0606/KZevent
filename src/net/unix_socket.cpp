#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/unix_socket.hpp"

namespace kzevent::net::unix {
std::shared_ptr<UnixClient> UnixClient::make_unix_client(core::Loop &loop,
                                                        const InetAddr &local) {
  if (local.get_sockaddr()->sa_family != AF_UNIX) {
    /* unix socket 只支持 path和abstract */
    KZ_LOG_ERROR("unix client err: unsupported protocol");
    return {};
  }

  struct EnableMakeShared : public UnixClient {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : UnixClient(loop, local) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop, local);
  return ret;
}

void UnixClient::set_split_cb(SplitCallBack cb) noexcept {
  split_cb_ = std::move(cb);
}

void UnixClient::set_fragment_cb(FragmentCallBack cb) noexcept {
  fragment_cb_ = std::move(cb);
}

void UnixClient::set_error_cb(DisconnectCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

void UnixClient::set_disconnect_cb(DisconnectCallBack cb) noexcept {
  disconnect_cb_ = std::move(cb);
}

UnixClient::UnixClient(core::Loop &loop, const InetAddr &local)
    : StreamClientSocket(loop, local), path_(local.get_ip_or_path()) {}

ssize_t UnixClient::on_split() {
  if (split_cb_ == nullptr) {
    /* 未注册切包回调 -> 不切分 */
    return 0;
  }

  const std::string_view slice{reinterpret_cast<char *>(recv_buf_.begin()),
                               static_cast<size_t>(recv_buf_.size())};

  return split_cb_(std::static_pointer_cast<UnixClient>(shared_from_this()),
                   slice);
}

void UnixClient::on_fragment(std::vector<uint8_t> fragment) {
  if (fragment_cb_ == nullptr) {
    /* 未注册执行回调 -> 不执行 */
    return;
  }

  auto task = [this, fragment = std::move(fragment)]() mutable {
    fragment_cb_(std::static_pointer_cast<UnixClient>(shared_from_this()),
                 std::move(fragment));
  };

  post_heavy_task(std::move(task));
}

void UnixClient::on_error(const int32_t err) {
  KZ_LOG_ERROR("unix client err: ", std::system_category().message(err));
  stop();

  if (error_cb_ == nullptr) {
    /* 未注册错误断连回调 -> 不处理 */
    return;
  }

  switch (err) {
  case ECONNREFUSED:
  case ENOENT:
  case EACCES:
  case ECONNRESET: {
    error_cb_(std::static_pointer_cast<UnixClient>(shared_from_this()));
    break;
  }

  default:
    break;
  }
}

void UnixClient::on_disconnect() {
  stop();

  if (disconnect_cb_ == nullptr) {
    /* 未注册正常断连回调 -> 不处理 */
    return;
  }
  disconnect_cb_(std::static_pointer_cast<UnixClient>(shared_from_this()));
}

std::shared_ptr<UnixServer> UnixServer::make_unix_server(core::Loop &loop,
                                                        const InetAddr &local) {
  if (local.get_sockaddr()->sa_family != AF_UNIX) {
    /* unix socket 只支持 unix path和abstract */
    KZ_LOG_ERROR("unix server err: unsupported protocol");
    return {};
  }

  struct EnableMakeShared : public UnixServer {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : UnixServer(loop, local) {}
  };

  auto ret = std::make_shared<EnableMakeShared>(loop, local);
  ret->start();
  return ret;
}

void UnixServer::set_split_cb(SplitCallBack cb) noexcept {
  split_cb_ = std::move(cb);
}

void UnixServer::set_fragment_cb(FragmentCallBack cb) noexcept {
  fragment_cb_ = std::move(cb);
}

void UnixServer::set_session_error_cb(DisconnectCallBack cb) noexcept {
  session_error_cb_ = std::move(cb);
}

void UnixServer::set_disconnect_cb(DisconnectCallBack cb) noexcept {
  disconnect_cb_ = std::move(cb);
}

UnixServer::UnixServer(core::Loop &loop, const InetAddr &local)
    : StreamServerSocket(loop, local), path_(local.get_ip_or_path()) {}

ssize_t UnixServer::on_split(const Session session) {
  if (split_cb_ == nullptr) {
    /* 未注册切包回调 -> 不切分 */
    return 0;
  }

  const std::string_view slice{
      reinterpret_cast<char *>(session->recv_buf_.begin()),
      static_cast<size_t>(session->recv_buf_.size())};

  return split_cb_(session, slice);
}

void UnixServer::on_fragment(std::vector<uint8_t> fragment, Session session) {
  if (fragment_cb_ == nullptr) {
    /* 未注册执行回调 -> 不执行 */
    return;
  }

  auto task = [this, fragment = std::move(fragment), session]() mutable {
    fragment_cb_(session, std::move(fragment));
  };

  session->post_heavy_task(std::move(task));
}

void UnixServer::on_session_error(const int32_t err, const Session session) {
  KZ_LOG_ERROR("unix client err: ", std::system_category().message(err));
  session->stop();

  if (session_error_cb_ == nullptr) {
    /* 未注册错误断连回调 -> 不处理 */
    return;
  }

  switch (err) {
  case ECONNREFUSED:
  case ENOENT:
  case EACCES:
  case ECONNRESET: {
    session_error_cb_(session);
    break;
  }

  default:
    break;
  }
}

void UnixServer::on_disconnect(const Session session) {
  session->stop();

  if (disconnect_cb_ == nullptr) {
    /* 未注册正常断连回调 -> 不处理 */
    return;
  }
  disconnect_cb_(session);
}
} // namespace kzevent::net::unix