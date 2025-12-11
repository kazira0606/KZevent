#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "kzevent/core.hpp"
#include "kzevent/net/inet.hpp"

namespace kzevent::net::udp {

/* 通用回调类型 */
using ErrorCallBack = std::function<void()>;

/*-------------------- UDP node  --------------------*/
class UdpNode : public UdpSocket {
  /* node 接收回调 */
  using NodeCallBack =
      std::function<void(const std::shared_ptr<UdpNode> &udp_node,
                         std::vector<uint8_t> data, InetAddr source)>;

public:
  ~UdpNode() override;

  /* 静态工厂 */
  static std::shared_ptr<UdpNode> make_udp_node(core::Loop &loop,
                                                const InetAddr &local);

  /* 方法 */
  using UdpSocket::post_send_task;

  using UdpSocket::post_heavy_task;

  using UdpSocket::start;

  using UdpSocket::stop;

  void set_read_cb(NodeCallBack cb) noexcept;

  void set_error_cb(ErrorCallBack cb) noexcept;

private:
  /* 构造函数 */
  UdpNode(core::Loop &loop, const InetAddr &local);

  /* 基类接口 */
  void on_read(std::vector<uint8_t> data, const InetAddr &source) override;

  void on_error() override;

  /* 回调 */
  NodeCallBack read_cb_{};
  ErrorCallBack error_cb_{};
};

/*-------------------- UDP client  --------------------*/
class UdpClient : public UdpSocket {
  /* client 接收回调 */
  using ClientCallBack =
      std::function<void(const std::shared_ptr<UdpClient> &udp_client,
                         std::vector<std::uint8_t> data)>;

public:
  ~UdpClient() override;

  /* 静态工厂 */
  static std::shared_ptr<UdpClient> make_udp_client(core::Loop &loop,
                                                    const InetAddr &local,
                                                    const InetAddr &source);

  /* 方法 */
  template <typename container> void post_send_task(container data);

  using UdpSocket::post_heavy_task;

  using UdpSocket::start;

  using UdpSocket::stop;

  void set_read_cb(ClientCallBack cb) noexcept;

  void set_error_cb(ErrorCallBack cb) noexcept;

private:
  /* 构造函数 */
  UdpClient(core::Loop &loop, const InetAddr &local, const InetAddr &source);

  /* 基类接口 */
  void on_read(std::vector<uint8_t> data, const InetAddr &source) override;

  void on_error() override;

  /* 回调 */
  ClientCallBack read_cb_{};
  ErrorCallBack error_cb_{};

  /* 对端地址 */
  InetAddr source_;
};

/*-------------------- UDP server  --------------------*/
class UdpServer : public UdpSocket {
public:
  /* 业务会话 */
  class UdpSession {
    friend class UdpServer;

  public:
    ~UdpSession() = default;

    /* 静态工厂 */
    static std::shared_ptr<UdpSession>
    make_udp_session(const std::shared_ptr<UdpServer> &server,
                     const InetAddr &source,
                     std::chrono::steady_clock::time_point time_stamp);

    /* 接口 */
    void set_user_context(std::shared_ptr<void> user_context) noexcept;

    [[nodiscard]] std::shared_ptr<void> get_user_context() const noexcept;

    template <typename container> void post_send_task(container data);

    template <typename Fun> void post_heavy_task(Fun fun);

  private:
    UdpSession(const std::shared_ptr<UdpServer> &server, const InetAddr &source,
               std::chrono::steady_clock::time_point time_stamp);

    /* 会话上下文 */
    std::shared_ptr<void> user_context_{};
    InetAddr source_;

    /* 时间戳 */
    std::chrono::steady_clock::time_point time_stamp_{};

    /* server指针 */
    std::weak_ptr<UdpServer> server_{};
  };

  /* server新建session和接收回调 */
  using ServerCallBack =
      std::function<void(const std::shared_ptr<UdpSession> &udp_session,
                         std::vector<std::uint8_t> data)>;

  ~UdpServer() override;

  /* 静态工厂 */
  static std::shared_ptr<UdpServer>
  make_udp_server(core::Loop &loop, const InetAddr &local,
                  uint64_t session_timeout_ms = 60000);

  /* 接口 */
  void start();

  using UdpSocket::stop;

  void set_new_session_cb(ServerCallBack cb) noexcept;

  void set_read_cb(ServerCallBack cb) noexcept;

  void set_error_cb(ErrorCallBack cb) noexcept;

private:
  /* 构造函数 */
  UdpServer(core::Loop &loop, const InetAddr &local,
            uint64_t session_timeout_ms);

  using UdpSocket::post_send_task;

  using UdpSocket::post_heavy_task;

  /* 基类接口 */
  void on_read(std::vector<uint8_t> data, const InetAddr &source) override;

  void on_error() override;

  /* 回调 */
  ServerCallBack new_session_cb_{};
  ServerCallBack read_cb_{};
  ErrorCallBack error_cb_{};

  /* 会话管理 */
  std::unordered_map<InetAddr, std::shared_ptr<UdpSession>> sessions_{};
  
  /* 默认超时时长1min */
  core::LoopChannel timer_channel_;
  uint64_t session_timeout_ms_{60000};
};

/*-------------------- 模板实现  --------------------*/
template <typename container> void UdpClient::post_send_task(container data) {
  UdpSocket::post_send_task(std::move(data), source_);
}

template <typename container>
void UdpServer::UdpSession::post_send_task(container data) {
  auto server = server_.lock();
  if (server == nullptr) {
    return;
  }
  server->post_send_task(std::move(data), source_);
}

template <typename Fun> void UdpServer::UdpSession::post_heavy_task(Fun fun) {
  auto server = server_.lock();
  if (server == nullptr) {
    return;
  }
  server->post_heavy_task(std::move(fun));
}
} // namespace kzevent::net::udp