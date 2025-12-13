#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <vector>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/sys_error.hpp"

namespace kzevent::net {
/*-------------------- 网络地址  --------------------*/
bool InetAddr::operator==(const InetAddr &other) const noexcept {
  if (addr_storage_.ss_family != other.addr_storage_.ss_family) {
    return false;
  }

  switch (addr_storage_.ss_family) {
  case AF_INET: {
    const auto self_ipv4_in{
        reinterpret_cast<const sockaddr_in *>(&addr_storage_)};
    const auto other_ipv4_in{
        reinterpret_cast<const sockaddr_in *>(&other.addr_storage_)};

    /* 比较port和ip */
    return self_ipv4_in->sin_port == other_ipv4_in->sin_port &&
           self_ipv4_in->sin_addr.s_addr == other_ipv4_in->sin_addr.s_addr;
  }

  case AF_INET6: {
    const auto self_ipv6_in{
        reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};
    const auto other_ipv6_in{
        reinterpret_cast<const sockaddr_in6 *>(&other.addr_storage_)};

    /* 比较port和ip和scope */
    return self_ipv6_in->sin6_port == other_ipv6_in->sin6_port &&
           self_ipv6_in->sin6_scope_id == other_ipv6_in->sin6_scope_id &&
           std::memcmp(&self_ipv6_in->sin6_addr, &other_ipv6_in->sin6_addr,
                       sizeof(in6_addr)) == 0;
  }

  case AF_UNIX: {
    if (socklen_ != other.socklen_) {
      return false;
    }

    const auto self_un_in{
        reinterpret_cast<const sockaddr_un *>(&addr_storage_)};
    const auto other_un_in{
        reinterpret_cast<const sockaddr_un *>(&other.addr_storage_)};

    constexpr auto base{
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
    if (socklen_ <= base) {
      return false;
    }
    const auto sun_len{static_cast<size_t>(socklen_ - base)};

    /* 比较sun_path */
    return std::memcmp(self_un_in->sun_path, other_un_in->sun_path, sun_len) ==
           0;
  }

  default:
    return false;
  }
}

bool InetAddr::operator!=(const InetAddr &other) const noexcept {
  return !(*this == other);
}

size_t InetAddr::hash() const noexcept {
  const std::string_view bytes(reinterpret_cast<const char *>(&addr_storage_),
                               socklen_);
  return std::hash<std::string_view>{}(bytes);
}

/* 静态工厂 */
std::optional<InetAddr> InetAddr::make_ipv4(const std::string &ip,
                                            const uint16_t port) {
  InetAddr ipv4{};
  const auto ipv4_in{reinterpret_cast<sockaddr_in *>(&ipv4.addr_storage_)};

  if (const auto ret = inet_pton(AF_INET, ip.c_str(), &ipv4_in->sin_addr);
      ret == 0) {
    /* 0 无效ip */
    KZ_LOG_ERROR("invalid ipv4 address: ", ip.c_str());
    return std::nullopt;
  } else if (ret == -1) {
    /* -1 系统错误 */
    sys_error::error();
    return std::nullopt;
  }

  /* ipv4 */
  ipv4_in->sin_family = AF_INET;
  ipv4_in->sin_port = htons(port);
  ipv4.socklen_ = sizeof(sockaddr_in);

  return ipv4;
}

std::optional<InetAddr> InetAddr::make_ipv6(const std::string &ip,
                                            const uint16_t port,
                                            const uint32_t scope_id) {
  InetAddr ipv6{};
  const auto ipv6_in{reinterpret_cast<sockaddr_in6 *>(&ipv6.addr_storage_)};

  if (const auto ret = inet_pton(AF_INET6, ip.c_str(), &ipv6_in->sin6_addr);
      ret == 0) {
    /* 0 无效ip */
    KZ_LOG_ERROR("invalid ipv6 address: ", ip.c_str());
    return std::nullopt;
  } else if (ret == -1) {
    /* -1 系统错误 */
    sys_error::error();
    return std::nullopt;
  }

  /* ipv6 */
  ipv6_in->sin6_family = AF_INET6;
  ipv6_in->sin6_port = htons(port);
  ipv6_in->sin6_scope_id = scope_id;
  ipv6.socklen_ = sizeof(sockaddr_in6);

  return ipv6;
}

std::optional<InetAddr> InetAddr::make_unix(const std::string &path) {
  if (path.find('\0') != std::string::npos) {
    /* 不允许有NUL */
    KZ_LOG_ERROR("unix socket name/path contains NUL");
    return std::nullopt;
  }

  if (path.empty()) {
    /* 不允许无名 */
    KZ_LOG_ERROR("unix socket name/path is empty");
    return std::nullopt;
  }

  InetAddr unix{};
  const auto un_in{reinterpret_cast<sockaddr_un *>(&unix.addr_storage_)};
  constexpr auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};

  if (path.size() + 1 > sizeof(un_in->sun_path)) {
    /* 字符串长度过长 */
    KZ_LOG_ERROR("unix socket path too long: ", path.c_str());
    return std::nullopt;
  }

  un_in->sun_family = AF_UNIX;

  /* 文件路径型，c风格字符串 */
  path.copy(un_in->sun_path, path.size());
  un_in->sun_path[path.size()] = '\0';
  unix.socklen_ = static_cast<socklen_t>(base + path.size() + 1);

  return unix;
}

std::optional<InetAddr> InetAddr::make_abstract(const std::string &name) {
  if (name.find('\0') != std::string::npos) {
    /* 不允许有NUL */
    KZ_LOG_ERROR("unix socket name/path contains NUL");
    return std::nullopt;
  }

  if (name.empty()) {
    /* 不允许无名 */
    KZ_LOG_ERROR("unix socket name/path is empty");
    return std::nullopt;
  }

  InetAddr abstract{};
  const auto un_in{reinterpret_cast<sockaddr_un *>(&abstract.addr_storage_)};
  constexpr auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};

  if (name.size() + 1 > sizeof(un_in->sun_path)) {
    /* 字符串长度过长 */
    KZ_LOG_ERROR("abstract socket name too long: ", name.c_str());
    return std::nullopt;
  }

  un_in->sun_family = AF_UNIX;

  /* 抽象型开头'\0'占位，结尾无需'\0' */
  un_in->sun_path[0] = '\0';
  name.copy(un_in->sun_path + 1, name.size());
  abstract.socklen_ = static_cast<socklen_t>(base + 1 + name.size());

  return abstract;
}

std::optional<InetAddr> InetAddr::make_from_sockaddr(const sockaddr *addr,
                                                     const socklen_t len) {
  if (addr == nullptr) {
    KZ_LOG_ERROR("null sockaddr");
    return std::nullopt;
  }

  switch (addr->sa_family) {
  case AF_INET: {
    if (len < static_cast<socklen_t>(sizeof(sockaddr_in))) {
      KZ_LOG_ERROR("invalid sockaddr_in length");
      return std::nullopt;
    }

    /* ipv4反序列化 */
    sockaddr_storage storage{};
    std::memcpy(&storage, addr, sizeof(sockaddr_in));
    return InetAddr{storage, sizeof(sockaddr_in)};
  }

  case AF_INET6: {
    if (len < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
      KZ_LOG_ERROR("invalid sockaddr_in6 length");
      return std::nullopt;
    }

    /* ipv6反序列化 */
    sockaddr_storage storage{};
    std::memcpy(&storage, addr, sizeof(sockaddr_in6));
    return InetAddr{storage, sizeof(sockaddr_in6)};
  }

  case AF_UNIX: {
    constexpr auto base{
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};

    if (len <= base || len > static_cast<socklen_t>(sizeof(sockaddr_un))) {
      /* 不允许无名/过长 */
      KZ_LOG_ERROR("unix socket name/path is empty or too long");
      return std::nullopt;
    }

    const auto sun_len{static_cast<size_t>(len - base)};

    const auto un_in{reinterpret_cast<const sockaddr_un *>(addr)};

    /* unix反序列化 */
    if (un_in->sun_path[0] == '\0') {
      /* 抽象型 */
      auto name{std::string{un_in->sun_path + 1, sun_len - 1}};

      while (!name.empty() && name.back() == '\0') {
        /* 去除冗余'\0' */
        name.pop_back();
      }

      return make_abstract(name);
    }

    /* 路径型 */
    auto path{std::string{un_in->sun_path, sun_len}};

    while (!path.empty() && path.back() == '\0') {
      /* 去除冗余'\0' */
      path.pop_back();
    }

    return make_unix(path);
  }

  default:
    return std::nullopt;
  }
}

/* sys交互接口 */
const sockaddr *InetAddr::get_sockaddr() const noexcept {
  return reinterpret_cast<const sockaddr *>(&addr_storage_);
}

socklen_t InetAddr::get_socklen() const noexcept { return socklen_; }

/* 信息接口 */
std::string InetAddr::get_ip_or_path() const {
  switch (addr_storage_.ss_family) {
  case AF_INET: {
    /* ipv4 */
    const auto ipv4_in{reinterpret_cast<const sockaddr_in *>(&addr_storage_)};

    std::string ip_buf(INET_ADDRSTRLEN, '\0');
    if (const auto ret = inet_ntop(AF_INET, &ipv4_in->sin_addr, ip_buf.data(),
                                   ip_buf.size());
        ret == nullptr) {
      sys_error::error();
      return {};
    }

    /* 去除冗余'\0' */
    ip_buf.resize(std::strlen(ip_buf.c_str()));
    return ip_buf;
  }

  case AF_INET6: {
    /* ipv6 */
    const auto ipv6_in{reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};

    std::string ip_buf(INET6_ADDRSTRLEN, '\0');
    if (const auto ret = inet_ntop(AF_INET6, &ipv6_in->sin6_addr, ip_buf.data(),
                                   ip_buf.size());
        ret == nullptr) {
      sys_error::error();
      return {};
    }

    /* 去除冗余'\0' */
    ip_buf.resize(std::strlen(ip_buf.c_str()));
    return ip_buf;
  }

  case AF_UNIX: {
    const auto un_in{reinterpret_cast<const sockaddr_un *>(&addr_storage_)};

    constexpr auto base{
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
    if (socklen_ <= base) {
      return {};
    }
    const auto sun_len{static_cast<size_t>(socklen_ - base)};

    if (un_in->sun_path[0] == '\0') {
      /* 抽象型 */
      return std::string{"@"} + std::string{un_in->sun_path + 1, sun_len - 1};
    } else {
      /* 路径型 */
      return std::string{un_in->sun_path};
    }
  }

  default:
    return {};
  }
}

uint16_t InetAddr::get_port() const {
  switch (addr_storage_.ss_family) {
  case AF_INET: {
    const auto ipv4_in{reinterpret_cast<const sockaddr_in *>(&addr_storage_)};
    return ntohs(ipv4_in->sin_port);
  }

  case AF_INET6: {
    const auto ipv6_in{reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};
    return ntohs(ipv6_in->sin6_port);
  }

  default: {
    /* unix/abstract/失败 */
    return 0;
  }
  }
}

/* ipv4/ipv6 反序列化构造函数 */
InetAddr::InetAddr(const sockaddr_storage &addr_storage,
                   const socklen_t socklen) noexcept {
  addr_storage_ = addr_storage;
  socklen_ = socklen;
}
/*-------------------- Channel工厂  --------------------*/
std::optional<core::LoopChannel> make_udp_channel(core::Loop &loop,
                                                  const InetAddr &addr) {
  const auto fd = socket(addr.get_sockaddr()->sa_family,
                         SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }
  /* 立即移交fd便于RAII */
  core::LoopChannel udp_channel(loop, fd);

  /* ipv6 socket禁用ipv4监听 */
  if (addr.get_sockaddr()->sa_family == AF_INET6) {
    constexpr int32_t on{1};
    if (const auto ret =
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        ret == -1) {
      sys_error::error();
      return std::nullopt;
    }
  }

  if (const auto ret = bind(fd, addr.get_sockaddr(), addr.get_socklen());
      ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return udp_channel;
}

std::optional<core::LoopChannel> make_timer_channel(core::Loop &loop,
                                                    uint64_t timeout_ms) {
  const auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }

  /* 立即移交fd便于RAII */
  core::LoopChannel timer_channel(loop, fd);

  const auto sec = timeout_ms / 1000;
  const auto nsec = (timeout_ms % 1000) * 1000000;

  struct itimerspec ts {};
  /* 首次延迟 */
  ts.it_value.tv_sec = static_cast<time_t>(sec);
  ts.it_value.tv_nsec = static_cast<long>(nsec);

  /* 后续延迟 */
  ts.it_interval.tv_sec = static_cast<time_t>(sec);
  ts.it_interval.tv_nsec = static_cast<long>(nsec);

  if (const auto ret = timerfd_settime(fd, 0, &ts, nullptr); ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return timer_channel;
}
/*-------------------- 网络基类  --------------------*/
UdpSocket::UdpSocket(core::Loop &loop, const InetAddr &local)
    : udp_channel_([&]() -> core::LoopChannel {
        auto ch = make_udp_channel(loop, local);

        if (!ch.has_value()) {
          throw std::runtime_error("make udp channel failed");
        }

        return std::move(ch).value();
      }()) {}

void UdpSocket::start() {
  bool expected{false};
  if (!started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto cb = [this](const core::EventType event_types) {
    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      /* 可读事件,循环读取缓冲区所有的包 */
      while (true) {
        const auto [ret, source_addr] = udp_recv(udp_channel_, recv_buf_);
        if (!source_addr.has_value()) {
          /* EAGAIN或系统错误(有log) */
          break;
        }

        auto to_user =
            std::vector<uint8_t>{recv_buf_.begin(), recv_buf_.begin() + ret};
        on_read(std::move(to_user), source_addr.value());
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件 */
      on_error();
    }

    /* 其他事件暂不支持 */
  };

  udp_channel_.update_event(weak_from_this(),
                            core::EventType::kRead | core::EventType::kError,
                            core::EventMode::kDefault, std::move(cb));
}

void UdpSocket::stop() {
  started_ = false;
  udp_channel_.disable_event();
}
} // namespace kzevent::net