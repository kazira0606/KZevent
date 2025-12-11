#include <cassert>
#include <cstdint>
#include <memory>

#include <arpa/inet.h>
#include <utility>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

namespace kzevent::net::udp {
/*-------------------- UDP node  --------------------*/
UdpNode::~UdpNode() { stop(); }

std::shared_ptr<UdpNode> UdpNode::make_udp_node(core::Loop &loop,
                                                const InetAddr &local) {
  struct EnableMakeShared : public UdpNode {
    EnableMakeShared(core::Loop &loop, const InetAddr &local) : UdpNode(loop, local) {}
  };
  return std::make_shared<EnableMakeShared>(loop, local);
}

void UdpNode::set_read_cb(NodeCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

void UdpNode::set_error_cb(ErrorCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

UdpNode::UdpNode(core::Loop &loop, const InetAddr &local) : UdpSocket(loop, local) {}

void UdpNode::on_read(std::vector<uint8_t> data, const InetAddr &source) {
  if (read_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }
  read_cb_(std::static_pointer_cast<UdpNode>(shared_from_this()),
           std::move(data), source);
}

void UdpNode::on_error() {
  if (error_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }
  error_cb_();
}

/*-------------------- UDP client  --------------------*/
UdpClient::~UdpClient() { stop(); }

std::shared_ptr<UdpClient>
UdpClient::make_udp_client(core::Loop &loop, const InetAddr &local, const InetAddr &source) {
  struct EnableMakeShared : public UdpClient {
    EnableMakeShared(core::Loop &loop, const InetAddr &local, const InetAddr &source)
        : UdpClient(loop, local, source) {}
  };
  return std::make_shared<EnableMakeShared>(loop, local, source);
}

UdpClient::UdpClient(core::Loop &loop, const InetAddr &local,
                     const InetAddr &source)
    : UdpSocket(loop, local), source_(source) {}

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
    KZ_LOG_INFO("UDP client read data from unexpected source");
    return;
  }

  read_cb_(std::static_pointer_cast<UdpClient>(shared_from_this()),
           std::move(data));
}

void UdpClient::on_error() {
  if (error_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }
  error_cb_();
}

/*-------------------- UDP session  --------------------*/
std::shared_ptr<UdpServer::UdpSession> UdpServer::UdpSession::make_udp_session(
    const std::shared_ptr<UdpServer> &server, const InetAddr &source) {
  struct EnableMakeShared : public UdpServer::UdpSession {
    EnableMakeShared(const std::shared_ptr<UdpServer> &server, const InetAddr &source)
        : UdpServer::UdpSession(server, source) {}
  };
  return std::make_shared<EnableMakeShared>(server, source);
}

void UdpServer::UdpSession::set_user_context(
    std::shared_ptr<void> user_context) noexcept {
  user_context_ = std::move(user_context);
}

[[nodiscard]] std::shared_ptr<void>
UdpServer::UdpSession::get_user_context() const noexcept {
  return user_context_;
}

UdpServer::UdpSession::UdpSession(const std::shared_ptr<UdpServer> &server,
                                  const InetAddr &source)
    : source_(source), server_(server) {}

/*-------------------- UDP server  --------------------*/
UdpServer::~UdpServer() { stop(); }
std::shared_ptr<UdpServer> UdpServer::make_udp_server(core::Loop &loop,
                                                      const InetAddr &local) {
  struct EnableMakeShared : public UdpServer {
    EnableMakeShared(core::Loop &loop, const InetAddr &local)
        : UdpServer(loop, local) {}
  };
  return std::make_shared<EnableMakeShared>(loop, local);
}

void UdpServer::set_new_session_cb(ServerCallBack cb) noexcept {
  new_session_cb_ = std::move(cb);
}

void UdpServer::set_read_cb(ServerCallBack cb) noexcept {
  read_cb_ = std::move(cb);
}

void UdpServer::set_error_cb(ErrorCallBack cb) noexcept {
  error_cb_ = std::move(cb);
}

UdpServer::UdpServer(core::Loop &loop, const InetAddr &local)
    : UdpSocket(loop, local) {}

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
        std::static_pointer_cast<UdpServer>(shared_from_this()), source);

    /* 加入管理 */
    sessions_.emplace(source, new_session);

    if (new_session_cb_ != nullptr) {
      /* 执行新连接回调 */
      new_session_cb_(new_session, std::move(data));

    } else {
      /* 未注册新连接回调则默认执行数据回调 */
      read_cb_(new_session, std::move(data));
    }
    return;
  }

  /* 已有会话 */
  if (read_cb_ == nullptr) {
    return;
  }
  read_cb_(it->second, std::move(data));
}

void UdpServer::on_error() {
  if (error_cb_ == nullptr) {
    /* 还未注册回调 */
    return;
  }
  error_cb_();
}
} // namespace kzevent::net::udp