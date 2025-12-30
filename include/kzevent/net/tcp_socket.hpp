#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <sys/types.h>

#include "kzevent/core.hpp"
#include "kzevent/net/inet.hpp"

namespace kzevent::net::tcp {
/*-------------------- TCP client  --------------------*/
class TcpClient : public StreamClientSocket {
  /* TCP流回调，返回本次解析某帧消耗的字节数 */
  using SplitCallBack = std::function<ssize_t(
      const std::shared_ptr<TcpClient> &session, std::string_view slice)>;

  /* TCP帧回调，形参为split解析的帧 */
  using FragmentCallBack =
      std::function<void(const std::shared_ptr<TcpClient> &session,
                         std::vector<uint8_t> fragment)>;

  /* TCP失败/异常/正常断连断连回调 */
  using DisconnectCallBack =
      std::function<void(const std::shared_ptr<TcpClient> &session)>;

public:
  /* 静态工厂 */
  static std::shared_ptr<TcpClient> make_tcp_client(core::Loop &loop,
                                                    const InetAddr &local);

  ~TcpClient() override = default;

  /* 方法 */
  using StreamClientSocket::post_send_task;

  using StreamClientSocket::post_heavy_task;

  /* connect后自动start */
  using StreamClientSocket::connect;

  using StreamClientSocket::start;

  using StreamClientSocket::stop;

  void set_split_cb(SplitCallBack cb) noexcept;

  void set_fragment_cb(FragmentCallBack cb) noexcept;

  void set_error_cb(DisconnectCallBack cb) noexcept;

  void set_disconnect_cb(DisconnectCallBack cb) noexcept;

private:
  /* 构造函数 */
  TcpClient(core::Loop &loop, const InetAddr &local);

  /* 基类接口 */
  ssize_t on_split() override;

  void on_fragment(std::vector<uint8_t> fragment) override;

  void on_error(int32_t err) override;

  void on_disconnect() override;

  /* 回调 */
  SplitCallBack split_cb_{};
  FragmentCallBack fragment_cb_{};
  DisconnectCallBack error_cb_{};
  DisconnectCallBack disconnect_cb_{};
};

/*-------------------- TCP server  --------------------*/
class TcpServer : public StreamServerSocket {
public:
  /* TCP会话 */
  using Session = std::shared_ptr<StreamSession>;

  /* TCP流回调，返回本次解析某帧消耗的字节数 */
  using SplitCallBack =
      std::function<ssize_t(const Session &session, std::string_view slice)>;

  /* TCP帧回调，形参为split解析的帧 */
  using FragmentCallBack = std::function<void(const Session &session,
                                              std::vector<uint8_t> fragment)>;

  /* TCP失败/异常/正常断连断连回调 */
  using DisconnectCallBack = std::function<void(const Session &session)>;

  ~TcpServer() override = default;

  /* 静态工厂 */
  static std::shared_ptr<TcpServer> make_tcp_server(core::Loop &loop,
                                                    const InetAddr &local);

  /* 方法 */
  using StreamServerSocket::start;

  using StreamServerSocket::stop;

  void set_split_cb(SplitCallBack cb) noexcept;

  void set_fragment_cb(FragmentCallBack cb) noexcept;

  void set_session_error_cb(DisconnectCallBack cb) noexcept;

  void set_disconnect_cb(DisconnectCallBack cb) noexcept;

private:
  /* 构造函数 */
  TcpServer(core::Loop &loop, const InetAddr &local);

  /* 基类接口 */
  ssize_t on_split(Session session) override;

  void on_fragment(std::vector<uint8_t> fragment, Session session) override;

  void on_session_error(int32_t err, Session session) override;

  void on_disconnect(Session session) override;

  /* 回调 */
  SplitCallBack split_cb_{};
  FragmentCallBack fragment_cb_{};
  DisconnectCallBack session_error_cb_{};
  DisconnectCallBack disconnect_cb_{};
};
} // namespace kzevent::net::tcp
