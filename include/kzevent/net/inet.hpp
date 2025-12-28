#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <bits/types/struct_iovec.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "kzevent/core.hpp"
#include "kzevent/stream_buffer.hpp"
#include "kzevent/sys_error.hpp"

namespace kzevent::net {
/*-------------------- 网络地址  --------------------*/
class InetAddr {
public:
  ~InetAddr() = default;

  bool operator==(const InetAddr &other) const noexcept;

  bool operator!=(const InetAddr &other) const noexcept;

  [[nodiscard]] size_t hash() const noexcept;

  /* 静态工厂 */

  static std::optional<InetAddr> make_ipv4(const std::string &ip,
                                           uint16_t port);

  static std::optional<InetAddr> make_ipv6(const std::string &ip, uint16_t port,
                                           uint32_t scope_id = 0);

  static std::optional<InetAddr> make_unix(const std::string &path);

  static std::optional<InetAddr> make_abstract(const std::string &name);

  static std::optional<InetAddr> make_from_sockaddr(const sockaddr *addr,
                                                    socklen_t len);

  /* sys交互接口 */
  [[nodiscard]] const sockaddr *get_sockaddr() const noexcept;

  [[nodiscard]] socklen_t get_socklen() const noexcept;

  /* 信息接口 */
  [[nodiscard]] std::string get_ip_or_path() const;

  [[nodiscard]] uint16_t get_port() const;

private:
  InetAddr() = default;

  /* ipv4/ipv6 反序列化构造函数 注 unix反序列化直接使用工厂->保证库统一格式 */
  InetAddr(const sockaddr_storage &addr_storage, socklen_t socklen) noexcept;

  sockaddr_storage addr_storage_{};
  socklen_t socklen_{};
};

/*-------------------- Channel工厂  --------------------*/
[[nodiscard]] std::optional<core::LoopChannel>
make_dgram_channel(core::Loop &loop, const InetAddr &addr);

[[nodiscard]] std::optional<core::LoopChannel>
make_stream_server_channel(core::Loop &loop, const InetAddr &addr);

[[nodiscard]] std::optional<core::LoopChannel>
make_stream_client_channel(core::Loop &loop, const InetAddr &addr);

[[nodiscard]] std::optional<core::LoopChannel>
make_timer_channel(core::Loop &loop, uint64_t timeout_ms = 0,
                   uint64_t repeat_ms = 0);

/*-------------------- 模板限制  --------------------*/
/* 发送函数允许的模板类型 */
namespace detail {
template <typename T> struct is_allowed_for_send : std::false_type {};

template <>
struct is_allowed_for_send<std::vector<std::uint8_t>> : std::true_type {};

template <std::size_t N>
struct is_allowed_for_send<std::array<std::uint8_t, N>> : std::true_type {};

template <> struct is_allowed_for_send<std::string> : std::true_type {};

template <typename T>
inline constexpr bool is_allowed_for_send_v =
    is_allowed_for_send<std::decay_t<T>>::value;
} // namespace detail

/*-------------------- 网络接口  --------------------*/
/* dgram */
template <typename container>
void dgram_send(core::LoopChannel &channel, const container &data,
                const InetAddr &addr);

std::pair<ssize_t, std::optional<InetAddr>>
dgram_recv(core::LoopChannel &channel, std::array<uint8_t, UINT16_MAX> &buf);

/* stream */
enum class IoStatus { kSuccess, kTryAgain, kDisconnected, kError };

template <typename container>
std::pair<ssize_t, IoStatus> stream_send(core::LoopChannel &channel,
                                         const container &data);

IoStatus stream_send(core::LoopChannel &channel, core::StreamBuffer &buf);

IoStatus stream_recv(core::LoopChannel &channel, core::StreamBuffer &buf);

/*-------------------- 网络基类  --------------------*/
class DgramSocket : public std::enable_shared_from_this<DgramSocket> {
public:
  virtual ~DgramSocket() = default;

protected:
  DgramSocket(core::Loop &loop, const InetAddr &local);

  /* 方法 */
  template <typename container>
  void post_send_task(container data, const InetAddr &source);

  template <typename Fun> void post_heavy_task(Fun task);

  void start();

  void stop();

private:
  /* 接口 */
  virtual void on_read(std::vector<uint8_t> data, const InetAddr &source) = 0;

  virtual void on_error(int32_t err) = 0;

  core::LoopChannel dgram_channel_;

  /* 接收缓冲区 */
  std::array<uint8_t, UINT16_MAX> recv_buf_{};

  /* 启动标志 */
  std::atomic<bool> started_{false};
};

// class StreamServerSocket : public
// std::enable_shared_from_this<StreamServerSocket> { public:
//   virtual ~StreamServerSocket() = default;

// protected:
//   StreamServerSocket();
// };

class StreamClientSocket
    : public std::enable_shared_from_this<StreamClientSocket> {
public:
  virtual ~StreamClientSocket() = default;

protected:
  StreamClientSocket(core::Loop &loop, const InetAddr &local);

  /* 方法 */
  template <typename container> void post_send_task(container data);

  template <typename Fun> void post_heavy_task(Fun task);

  void connect(const InetAddr &addr, uint64_t timeout_ms);

  void start();

  void stop();

  /* 接收缓冲区 */
  core::StreamBuffer recv_buf_{UINT16_MAX};

  /* 发送缓冲区 */
  core::StreamBuffer send_buf_{UINT16_MAX};

private:
  /* 接口 */
  virtual ssize_t on_split() = 0;

  virtual void on_fragment(std::vector<uint8_t> fragment) = 0;

  virtual void on_error(int32_t err) = 0;

  virtual void on_disconnect() = 0;

  core::LoopChannel stream_channel_;

  /* 启动标志 */
  std::atomic<bool> started_{false};

  /* 连接标志 */
  enum class ConnectState { kDisconnected, kConnecting, kConnected };
  std::atomic<ConnectState> connected_{ConnectState::kDisconnected};
  core::LoopChannel connect_timer_channel_;
};

/*-------------------- 模板实现  --------------------*/
template <typename container>
void dgram_send(core::LoopChannel &channel, const container &data,
                const InetAddr &addr) {
  static_assert(detail::is_allowed_for_send_v<container>, "invalid data type");

  if (const auto ret = sendto(channel.get_fd(), data.data(), data.size(), 0,
                              addr.get_sockaddr(), addr.get_socklen());
      ret < 0) {
    /* 失败 */
    if (errno != EAGAIN) {
      sys_error::error();
    }
  }
}

template <typename container>
std::pair<ssize_t, IoStatus> stream_send(core::LoopChannel &channel,
                                         const container &data) {
  const auto ret =
      send(channel.get_fd(), data.data(), data.size(), MSG_NOSIGNAL);

  if (ret < 0) {
    if (errno == EAGAIN) {
      /* 缓冲区满 */
      return {ret, IoStatus::kTryAgain};
    }

    if (errno == EPIPE) {
      /* 对端关闭 */
      return {ret, IoStatus::kDisconnected};
    }

    /* 系统错误 */
    sys_error::error();
    return {ret, IoStatus::kError};
  }

  return {ret, IoStatus::kSuccess};
}

template <typename container>
void DgramSocket::post_send_task(container data, const InetAddr &source) {
  static_assert(detail::is_allowed_for_send_v<container>, "invalid data type");

  auto task = [this, data = std::move(data), source]() {
    dgram_send(dgram_channel_, data, source);
  };

  dgram_channel_.post_io_task(weak_from_this(), std::move(task));
}

template <typename Fun> void DgramSocket::post_heavy_task(Fun task) {
  dgram_channel_.post_heavy_task(weak_from_this(), std::move(task));
}

template <typename container>
void StreamClientSocket::post_send_task(container data) {
  static_assert(detail::is_allowed_for_send_v<container>, "invalid data type");

  auto task = [this, data = std::move(data)] {
    if (!send_buf_.empty() || connected_ != ConnectState::kConnected) {
      /* 发送缓冲区不为空/未连接 -> 进缓冲区排队 */
      send_buf_.insert(data.begin(), data.end());
      return;
    }

    /* 发送缓冲区为空且已连接 -> 直接发送 */
    const auto [ret, status] = stream_send(stream_channel_, data);

    switch (status) {
    case IoStatus::kSuccess: {
      /* 发送成功 */
      if (ret == data.size()) {
        /* 发送完毕 */
        break;
      }

      /* 剩余未发送数据 -> 进缓冲区排队 */
      send_buf_.insert(data.begin() + ret, data.end());
      /* 注册EPOLL可写事件 */
      const auto [old_types, old_modes] = stream_channel_.get_event_info();
      stream_channel_.update_event(old_types | core::EventType::kWrite,
                                   old_modes);
      break;
    }

    case IoStatus::kTryAgain: {
      /* 内核缓冲区满 -> 进缓冲区排队 */
      send_buf_.insert(data.begin(), data.end());
      /* 注册EPOLL可写事件 */
      const auto [old_types, old_modes] = stream_channel_.get_event_info();
      stream_channel_.update_event(old_types | core::EventType::kWrite,
                                   old_modes);
      break;
    }

    case IoStatus::kDisconnected: {
      /* 对端关闭 */
      send_buf_.clear();
      on_error(ECONNRESET);
      break;
    }

    case IoStatus::kError: {
      /* 系统错误已打日志 */
      break;
    }

    default:
      /* 不可达代码 */
      assert(false && "unknown io status");
      break;
    }
  };

  stream_channel_.post_io_task(weak_from_this(), std::move(task));
}

template <typename Fun> void StreamClientSocket::post_heavy_task(Fun task) {
  stream_channel_.post_heavy_task(weak_from_this(), std::move(task));
}
} // namespace kzevent::net

/* InetAddr 哈希函数 */
template <> struct std::hash<kzevent::net::InetAddr> {
  size_t operator()(const kzevent::net::InetAddr &addr) const noexcept {
    return addr.hash();
  };
};
