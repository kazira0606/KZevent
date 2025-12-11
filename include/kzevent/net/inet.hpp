#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>

#include "kzevent/core.hpp"
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
make_udp_channel(core::Loop &loop, const InetAddr &addr);

[[nodiscard]] std::optional<core::LoopChannel>
make_timer_channel(core::Loop &loop, uint64_t timeout_ms);

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
inline constexpr bool is_allowed_for_send_v = is_allowed_for_send<T>::value;

/* 接收函数允许的模板类型 */
template <typename T> struct is_allowed_for_recv : std::false_type {};

template <>
struct is_allowed_for_recv<std::vector<std::uint8_t>> : std::true_type {};

template <std::size_t N>
struct is_allowed_for_recv<std::array<std::uint8_t, N>> : std::true_type {};

template <typename T>
inline constexpr bool is_allowed_for_recv_v = is_allowed_for_recv<T>::value;

/* 模板类型断言 */
template <typename T> constexpr void is_allowed_type_for_send() {
  using CleanType = std::remove_cv_t<std::remove_reference_t<T>>;
  static_assert(is_allowed_for_send_v<CleanType>, "send type must be one of:\n"
                                                  "std::vector < std::uint8_t >"
                                                  "std::array<std::uint8_t, N>"
                                                  "std::string");
}

template <typename T> constexpr void is_allowed_type_for_recv() {
  using CleanType = std::remove_cv_t<std::remove_reference_t<T>>;
  static_assert(is_allowed_for_recv_v<CleanType>,
                "recv type must be one of:\n"
                "std::vector < std::uint8_t >"
                "std::array<std::uint8_t, N>");
}
} // namespace detail

/*-------------------- 网络接口  --------------------*/
/* udp */
template <typename container>
void udp_send(core::LoopChannel &channel, const container &data,
              const InetAddr &addr);

template <typename container>
std::pair<ssize_t, std::optional<InetAddr>> udp_recv(core::LoopChannel &channel,
                                                     container &buf);

/* tcp */
/* 暂未实现 */

/*-------------------- 网络基类  --------------------*/
class UdpSocket : public std::enable_shared_from_this<UdpSocket> {
public:
  virtual ~UdpSocket() = default;

protected:
  UdpSocket(core::Loop &loop, const InetAddr &local);

  /* 方法 */
  template <typename container>
  void post_send_task(container data, const InetAddr &source);

  template <typename Fun> void post_heavy_task(Fun task);

  void start();

  void stop() const;

private:
  /* 接口 */
  virtual void on_read(std::vector<uint8_t> data, const InetAddr &source) = 0;

  virtual void on_error() = 0;

  core::LoopChannel udp_channel_;

  /* 接收缓冲区 */
  std::array<uint8_t, 65536> recv_buf_{};
};

/*-------------------- 模板实现  --------------------*/
template <typename container>
void udp_send(core::LoopChannel &channel, const container &data,
              const InetAddr &addr) {
  detail::is_allowed_type_for_send<container>();

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
std::pair<ssize_t, std::optional<InetAddr>> udp_recv(core::LoopChannel &channel,
                                                     container &buf) {
  detail::is_allowed_type_for_recv<container>();

  sockaddr_storage source_addr{};
  socklen_t source_len{sizeof(source_addr)};

  if (const auto ret =
          recvfrom(channel.get_fd(), buf.data(), buf.size(), 0,
                   reinterpret_cast<sockaddr *>(&source_addr), &source_len);
      ret >= 0) {
    /* 成功（有截断风险） */
    return std::make_pair(
        ret, InetAddr::make_from_sockaddr(
                 reinterpret_cast<const sockaddr *>(&source_addr), source_len));
  } else {
    if (errno != EAGAIN) {
      /* 系统错误 */
      sys_error::error();
    }
    return std::make_pair(ret, std::nullopt);
  }
}

template <typename container>
void UdpSocket::post_send_task(container data, const InetAddr &source) {
  auto task = [this, data = std::move(data), source]() {
    udp_send(udp_channel_, data, source);
  };

  udp_channel_.post_io_task(weak_from_this(), std::move(task));
}

template <typename Fun> void UdpSocket::post_heavy_task(Fun task) {
  udp_channel_.post_heavy_task(weak_from_this(), std::move(task));
}
} // namespace kzevent::net

template <> struct std::hash<kzevent::net::InetAddr> {
  size_t operator()(const kzevent::net::InetAddr &addr) const noexcept {
    return addr.hash();
  };
};
