#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/sys_error.hpp"

namespace kzevent::net {
namespace {
bool reset_timer_fd(const int fd, const uint64_t timeout_ms) {
  itimerspec ts{};

  const auto sec = timeout_ms / 1000;
  const auto nsec = (timeout_ms % 1000) * 1000000;
  ts.it_value.tv_sec = static_cast<time_t>(sec);
  ts.it_value.tv_nsec = static_cast<long>(nsec);

  return ::timerfd_settime(fd, 0, &ts, nullptr) != -1;
}
} // namespace

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

    /* 比较port和ip */
    return self_ipv6_in->sin6_port == other_ipv6_in->sin6_port &&
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
  /* 初始种子 */
  std::size_t seed = 0;

  /* 叠加hash信息->协议 */
  hash_combine(seed, std::hash<uint16_t>{}(addr_storage_.ss_family));

  switch (addr_storage_.ss_family) {
  case AF_INET: {
    /* ipv4叠加hash信息->地址和端口 */
    const auto sock_in = reinterpret_cast<const sockaddr_in *>(&addr_storage_);

    hash_combine(seed, std::hash<uint32_t>{}(sock_in->sin_addr.s_addr));
    hash_combine(seed, std::hash<uint16_t>{}(sock_in->sin_port));
    break;
  }

  case AF_INET6: {
    /* ipv6叠加hash信息->地址和端口 */
    const auto sock_in = reinterpret_cast<const sockaddr_in6 *>(&addr_storage_);
    const std::string_view addr_bytes(
        reinterpret_cast<const char *>(sock_in->sin6_addr.s6_addr), 16);

    hash_combine(seed, std::hash<std::string_view>{}(addr_bytes));
    hash_combine(seed, std::hash<uint16_t>{}(sock_in->sin6_port));
    break;
  }

  case AF_UNIX: {
    /* unix叠加hash信息->路径 */
    const auto sock_un = reinterpret_cast<const sockaddr_un *>(&addr_storage_);

    if (constexpr auto base = offsetof(sockaddr_un, sun_path);
        socklen_ > base) {
      const std::string_view path(sock_un->sun_path, socklen_ - base);
      hash_combine(seed, std::hash<std::string_view>{}(path));
    }
    break;
  }

  default:
    /* 不可达 */
    return 0;
  }
  return seed;
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
std::optional<core::LoopChannel> make_dgram_channel(core::Loop &loop,
                                                    const InetAddr &addr) {
  const auto fd = socket(addr.get_sockaddr()->sa_family,
                         SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }
  /* 立即移交fd便于RAII */
  core::LoopChannel dgram_channel(loop, fd);

  /* ipv6 dgram socket禁用ipv4监听 */
  if (addr.get_sockaddr()->sa_family == AF_INET6) {
    constexpr int32_t on{1};
    if (const auto ret =
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        ret == -1) {
      sys_error::error();
      return std::nullopt;
    }
  }

  /* ipv4开启广播 */
  if (addr.get_sockaddr()->sa_family == AF_INET) {
    constexpr int32_t on{1};
    if (const auto ret =
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
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

  return dgram_channel;
}

std::optional<core::LoopChannel> make_stream_session_channel(core::Loop &loop,
                                                             const int32_t fd) {
  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }
  /* 立即移交fd便于RAII */
  core::LoopChannel stream_channel(loop, fd);
  return stream_channel;
}

std::optional<core::LoopChannel>
make_stream_server_channel(core::Loop &loop, const InetAddr &addr) {
  const auto fd = socket(addr.get_sockaddr()->sa_family,
                         SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }
  /* 立即移交fd便于RAII */
  core::LoopChannel stream_channel(loop, fd);

  /* 可重用 */
  constexpr int32_t opt{1};
  if (const auto ret =
          setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  /* ipv6 stream socket禁用ipv4监听 */
  if (addr.get_sockaddr()->sa_family == AF_INET6) {
    constexpr int32_t on{1};
    if (const auto ret =
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        ret == -1) {
      sys_error::error();
      return std::nullopt;
    }
  }

  /* bind */
  if (const auto ret = bind(fd, addr.get_sockaddr(), addr.get_socklen());
      ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  /* listen */
  if (const auto ret = listen(fd, SOMAXCONN); ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return stream_channel;
}

std::optional<core::LoopChannel>
make_stream_client_channel(core::Loop &loop, const InetAddr &addr) {
  const auto fd = socket(addr.get_sockaddr()->sa_family,
                         SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }
  /* 立即移交fd便于RAII */
  core::LoopChannel stream_channel(loop, fd);

  /* ipv6 stream socket禁用ipv4监听 */
  if (addr.get_sockaddr()->sa_family == AF_INET6) {
    constexpr int32_t on{1};
    if (const auto ret =
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        ret == -1) {
      sys_error::error();
      return std::nullopt;
    }
  }

  /* bind */
  if (const auto ret = bind(fd, addr.get_sockaddr(), addr.get_socklen());
      ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return stream_channel;
}

std::optional<core::LoopChannel> make_timer_channel(core::Loop &loop,
                                                    const uint64_t timeout_ms,
                                                    const uint64_t repeat_ms) {
  const auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

  if (fd == -1) {
    sys_error::error();
    return std::nullopt;
  }

  /* 立即移交fd便于RAII */
  core::LoopChannel timer_channel(loop, fd);

  if (timeout_ms == 0) {
    /* 只注册不计时 */
    return timer_channel;
  }

  const auto sec = timeout_ms / 1000;
  const auto nsec = (timeout_ms % 1000) * 1000000;

  struct itimerspec ts {};
  /* 首次延迟 */
  ts.it_value.tv_sec = static_cast<time_t>(sec);
  ts.it_value.tv_nsec = static_cast<long>(nsec);

  if (repeat_ms != 0) {
    /* 后续延迟 */
    const auto repeat_sec = repeat_ms / 1000;
    const auto repeat_nsec = (repeat_ms % 1000) * 1000000;

    ts.it_interval.tv_sec = static_cast<time_t>(repeat_sec);
    ts.it_interval.tv_nsec = static_cast<long>(repeat_nsec);
  }

  if (const auto ret = timerfd_settime(fd, 0, &ts, nullptr); ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return timer_channel;
}

/*-------------------- 网络接口  --------------------*/
std::pair<ssize_t, std::optional<InetAddr>>
dgram_recv(const core::LoopChannel &channel,
           std::array<uint8_t, UINT16_MAX> &buf) {
  sockaddr_storage source_addr{};
  socklen_t source_len{sizeof(source_addr)};

  const auto ret =
      recvfrom(channel.get_fd(), buf.data(), buf.size(), 0,
               reinterpret_cast<sockaddr *>(&source_addr), &source_len);
  if (ret < 0) {
    if (errno != EAGAIN) {
      /* 系统错误 */
      sys_error::error();
    }
    return {ret, std::nullopt};
  }

  return {ret,
          InetAddr::make_from_sockaddr(
              reinterpret_cast<const sockaddr *>(&source_addr), source_len)};
}

IoStatus stream_send(const core::LoopChannel &channel,
                     core::StreamBuffer &buf) {
  const auto ret =
      send(channel.get_fd(), buf.begin(), buf.size(), MSG_NOSIGNAL);

  if (ret < 0) {
    if (errno == EAGAIN) {
      /* 缓冲区满 */
      return IoStatus::kTryAgain;
    }

    if (errno == EPIPE) {
      /* 对端关闭 */
      return IoStatus::kDisconnected;
    }

    /* 系统错误 */
    sys_error::error();
    return IoStatus::kError;
  }

  /* 发送成功 */
  buf.pop(ret);
  return IoStatus::kSuccess;
}

IoStatus stream_recv(const core::LoopChannel &channel,
                     core::StreamBuffer &buf) {
  /* 栈缓冲区 */
  std::array<uint8_t, UINT16_MAX> ex_buf{};

  /* 记录 buf 空余空间 */
  const auto buf_space = buf.space();

  /* StreamBuffer + array 聚合缓冲 */
  const std::array<iovec, 2> iobuf{{{buf.end(), static_cast<size_t>(buf_space)},
                                    {ex_buf.data(), ex_buf.size()}}};

  const auto ret =
      readv(channel.get_fd(), iobuf.data(), static_cast<int32_t>(iobuf.size()));

  if (ret < 0) {
    if (errno == EAGAIN) {
      /* 缓冲区没有数据 */
      return IoStatus::kTryAgain;
    }

    if (errno == ECONNRESET) {
      /* 对端关闭 */
      return IoStatus::kDisconnected;
    }

    /* 系统错误 */
    sys_error::error();
    return IoStatus::kError;
  }

  if (ret == 0) {
    /* 对端关闭 */
    return IoStatus::kDisconnected;
  }

  if (ret > buf_space) {
    /* 写满了缓冲区 */
    buf.push(buf_space);
    /* 利用到了 ex_buf，复制进 buf */
    buf.insert(ex_buf.begin(), ex_buf.begin() + ret - buf_space);
  } else {
    /* 直接写入了 buf */
    buf.push(ret);
  }
  return IoStatus::kSuccess;
}

/*-------------------- 网络基类  --------------------*/
/* dgram socket基类 */
DgramSocket::DgramSocket(core::Loop &loop, const InetAddr &local)
    : dgram_channel_([&]() -> core::LoopChannel {
        auto ch = make_dgram_channel(loop, local);

        if (!ch.has_value()) {
          throw std::runtime_error("make udp channel failed");
        }

        return std::move(ch).value();
      }()) {}

void DgramSocket::joint_group(const InetAddr &addr) {
  const auto fd = dgram_channel_.get_fd();
  const auto sa = addr.get_sockaddr();

  switch (sa->sa_family) {
  case AF_INET: {
    ip_mreq group_addr{};
    const auto sa_in = reinterpret_cast<const sockaddr_in *>(sa);
    group_addr.imr_multiaddr = sa_in->sin_addr;
    group_addr.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group_addr,
                   sizeof(group_addr)) == -1) {
      sys_error::error();
    }

    return;
  }

  case AF_INET6: {
    ipv6_mreq group_addr{};
    const auto sa_in6 = reinterpret_cast<const sockaddr_in6 *>(sa);
    group_addr.ipv6mr_multiaddr = sa_in6->sin6_addr;
    group_addr.ipv6mr_interface = 0;

    if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group_addr,
                   sizeof(group_addr)) == -1) {
      sys_error::error();
    }

    return;
  }

  default:
    KZ_LOG_ERROR("invalid multicast address!");
  }
}

void DgramSocket::leave_group(const InetAddr &addr) {
  const auto fd = dgram_channel_.get_fd();
  const auto sa = addr.get_sockaddr();

  switch (sa->sa_family) {
  case AF_INET: {
    struct ip_mreq group_addr {};
    const auto sa_in = reinterpret_cast<const sockaddr_in *>(sa);

    group_addr.imr_multiaddr = sa_in->sin_addr;
    group_addr.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &group_addr,
                   sizeof(group_addr)) == -1) {
      sys_error::error();
    }
    break;
  }
    
  case AF_INET6: {
    struct ipv6_mreq group_addr {};
    const auto sa_in6 = reinterpret_cast<const sockaddr_in6 *>(sa);

    group_addr.ipv6mr_multiaddr = sa_in6->sin6_addr;
    group_addr.ipv6mr_interface = 0;

    if (setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &group_addr,
                   sizeof(group_addr)) == -1) {
      sys_error::error();
    }
    break;
  }
    
  default:
    KZ_LOG_ERROR("invalid multicast address!");
    break;
  }
}

void DgramSocket::start() {
  if (bool expected{false}; !started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto cb = [this](const core::EventType event_types) {
    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      /* 可读事件,循环读取缓冲区所有的包 */
      while (true) {
        const auto [len, source_addr] = dgram_recv(dgram_channel_, recv_buf_);
        if (!source_addr.has_value()) {
          /* EAGAIN或系统错误(有log) */
          break;
        }

        on_read({recv_buf_.begin(), recv_buf_.begin() + len},
                source_addr.value());
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件，清除 */
      int32_t err{0};
      socklen_t len = sizeof(err);
      getsockopt(dgram_channel_.get_fd(), SOL_SOCKET, SO_ERROR, &err, &len);

      if (err != 0) {
        on_error(err);
      }
    }

    /* 其他事件暂不支持 */
  };

  dgram_channel_.update_event(weak_from_this(),
                              core::EventType::kRead | core::EventType::kError,
                              core::EventMode::kDefault, std::move(cb));
}

void DgramSocket::stop() {
  started_ = false;
  dgram_channel_.disable_event();
}

/* stream sesion */
StreamServerSocket::StreamSession::StreamSession(
    core::Loop &loop, const int32_t fd,
    std::weak_ptr<StreamServerSocket> server)
    : recv_buf_(UINT16_MAX), send_buf_(UINT16_MAX), server_(std::move(server)),
      session_channel_([&]() -> core::LoopChannel {
        auto ch = make_stream_session_channel(loop, fd);

        if (!ch.has_value()) {
          throw std::runtime_error("make udp channel failed");
        }

        return std::move(ch).value();
      }()) {}

void StreamServerSocket::StreamSession::set_user_context(
    std::shared_ptr<void> user_context) noexcept {
  user_context_ = std::move(user_context);
}

[[nodiscard]] std::shared_ptr<void>
StreamServerSocket::StreamSession::get_user_context() const noexcept {
  return user_context_;
}

void StreamServerSocket::StreamSession::start() {
  auto cb = [this](const core::EventType event_types) {
    bool disconnected{false};

    /* 检查并延长所属的server的生命周期 */
    const auto server = server_.lock();
    if (server == nullptr) {
      return;
    }

    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      /* 可读事件,循环读取缓冲区所有的流直到EAGIN */
      while (true) {
        switch (const auto status = stream_recv(session_channel_, recv_buf_);
                status) {
        case IoStatus::kSuccess: {
          /* 读取到EGAIN */
          continue;
        }

        case IoStatus::kTryAgain: {
          break;
        }

        case IoStatus::kDisconnected: {
          /* 对端关闭 */
          disconnected = true;
          break;
        }

        case IoStatus::kError: {
          /* 系统错误，已打log */
          recv_buf_.clear();
          break;
        }

        default:
          /* 不可达代码 */
          assert(false && "unknown io status");
        }
        break;
      }
    }

    if ((event_types & core::EventType::kWrite) == core::EventType::kWrite) {
      /* 可写事件,循环写 send_buf_ 缓冲区的内容直到EAGIN */
      while (true) {
        switch (const auto status = stream_send(session_channel_, send_buf_);
                status) {
        case IoStatus::kSuccess: {
          /* 发送成功 */
          if (send_buf_.empty()) {
            /* 缓冲区数据发送完毕 */
            /* 取消注册EPOLL可写事件 */
            const auto [old_types, old_modes] =
                session_channel_.get_event_info();
            session_channel_.update_event(old_types & ~core::EventType::kWrite,
                                          old_modes);
            break;
          }
          continue;
        }

        case IoStatus::kTryAgain: {
          break;
        }

        case IoStatus::kDisconnected: {
          /* 对端关闭 */
          disconnected = true;
          break;
        }

        case IoStatus::kError: {
          /* 系统错误，已打log */
          recv_buf_.clear();
          break;
        }

        default:
          /* 不可达代码 */
          assert(false && "unknown io status");
        }
        break;
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件，清除 */
      int32_t err{0};
      socklen_t len = sizeof(err);
      getsockopt(session_channel_.get_fd(), SOL_SOCKET, SO_ERROR, &err, &len);

      if (err != 0 && err != ECONNRESET && err != EPIPE) {
        if (err == ECONNRESET || err == EPIPE) {
          disconnected = true;
        } else {
          recv_buf_.clear();
          send_buf_.clear();
          server->on_session_error(err, shared_from_this());
        }
      }
    }

    /* 其他事件暂不支持 */

    /* 处理缓冲区内所有数据 */
    while (true) {
      const auto consume_len = server->on_split(shared_from_this());
      if (consume_len <= 0) {
        /* 缓冲区已经没有完整的一帧 */
        break;
      }

      /* 切分了一帧 */
      server->on_fragment({recv_buf_.begin(), recv_buf_.begin() + consume_len},
                          shared_from_this());

      /* 移除已处理的数据 */
      recv_buf_.pop(static_cast<ssize_t>(consume_len));
    }

    if (disconnected) {
      /* 对端关闭 */
      recv_buf_.clear();
      send_buf_.clear();
      server->on_disconnect(shared_from_this());
    }
  };

  if (send_buf_.empty()) {
    /* 连接期间发送缓冲区无数据 */
    session_channel_.update_event(
        weak_from_this(), core::EventType::kRead | core::EventType::kError,
        core::EventMode::kDefault, std::move(cb));
  } else {
    /* 连接期间发送缓冲区有数据 */
    session_channel_.update_event(weak_from_this(),
                                  core::EventType::kRead |
                                      core::EventType::kWrite |
                                      core::EventType::kError,
                                  core::EventMode::kDefault, std::move(cb));
  }
}

void StreamServerSocket::StreamSession::stop() {
  session_channel_.disable_event();

  auto task = [this]() {
    const auto server = server_.lock();
    if (server == nullptr) {
      return;
    }
    server->sessions_.erase(session_channel_.get_fd());
  };

  session_channel_.post_io_task(weak_from_this(), std::move(task));
}

/* stream server socket 基类 */
StreamServerSocket::StreamServerSocket(core::Loop &loop, const InetAddr &local)
    : listen_channel_([&]() -> core::LoopChannel {
        auto ch = make_stream_server_channel(loop, local);

        if (!ch.has_value()) {
          throw std::runtime_error("make stream channel failed");
        }

        return std::move(ch).value();
      }()) {}

void StreamServerSocket::start() {
  if (bool expected{false}; !started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto cb = [this](const core::EventType event_types) {
    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      while (true) {
        /* accept事件 */
        sockaddr_storage client_addr{};
        socklen_t client_len{sizeof(client_addr)};

        if (const auto fd = accept4(listen_channel_.get_fd(),
                                    reinterpret_cast<sockaddr *>(&client_addr),
                                    &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            fd == -1) {
          if (errno == EAGAIN) {
            /* 缓冲区没有数据 */
            break;
          }

          if (errno != EAGAIN && errno != ECONNABORTED) {
            /* 系统错误 */
            sys_error::error();
            break;
          }
        } else {
          /* 新会话 */
          const auto session = std::make_shared<StreamSession>(
              *listen_channel_.get_loop(), fd, weak_from_this());
          sessions_.insert({session->session_channel_.get_fd(), session});

          /* 启动会话 */
          session->start();
        }
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件，清除 */
      int32_t err{0};
      socklen_t len = sizeof(err);
      getsockopt(listen_channel_.get_fd(), SOL_SOCKET, SO_ERROR, &err, &len);

      if (err != 0) {
        KZ_LOG_FATAL("listen socket fatal error",
                     std::error_code(errno, std::system_category()).message());
        stop();
      }
    }

    /* 其他事件暂不支持 */
  };

  listen_channel_.update_event(this->weak_from_this(),
                               core::EventType::kRead | core::EventType::kError,
                               core::EventMode::kDefault, std::move(cb));
}

void StreamServerSocket::stop() {
  started_ = false;
  listen_channel_.disable_event();
}

/* stream client socket基类 */
StreamClientSocket::StreamClientSocket(core::Loop &loop, const InetAddr &local)
    : recv_buf_(UINT16_MAX), send_buf_(UINT16_MAX),
      stream_channel_([&]() -> core::LoopChannel {
        auto ch = make_stream_client_channel(loop, local);

        if (!ch.has_value()) {
          throw std::runtime_error("make stream channel failed");
        }

        return std::move(ch).value();
      }()),
      connect_timer_channel_([&]() -> core::LoopChannel {
        auto ch = make_timer_channel(loop);

        if (!ch.has_value()) {
          throw std::runtime_error("make timer channel failed");
        }

        return std::move(ch).value();
      }()) {}

void StreamClientSocket::connect(const InetAddr &addr, uint64_t timeout_ms) {
  if (auto expected{ConnectState::kDisconnected};
      !connected_.compare_exchange_strong(expected,
                                          ConnectState::kConnecting)) {
    /* 已经连接/正在连接 */
    return;
  }

  if (const auto ret = ::connect(stream_channel_.get_fd(), addr.get_sockaddr(),
                                 addr.get_socklen());
      ret == 0) {
    /* 立即成功 */
    connected_ = ConnectState::kConnected;
    start();
    return;
  }

  if (errno != EINPROGRESS) {
    /* 连接失败 */
    sys_error::error();
    on_error(errno);
    connected_ = ConnectState::kDisconnected;
    return;
  }

  /* 检测连接 */
  auto socket_cb = [this](const core::EventType event_types) {
    if ((event_types & core::EventType::kWrite) == core::EventType::kWrite) {
      /* 可写事件，开始检测 */
      int err = 0;
      socklen_t len = sizeof(err);
      getsockopt(stream_channel_.get_fd(), SOL_SOCKET, SO_ERROR, &err, &len);

      if (err != 0) {
        /* 连接失败 */
        connected_ = ConnectState::kDisconnected;
        connect_timer_channel_.disable_event();
        on_error(err);
        return;
      }

      /* 成功 */
      connected_ = ConnectState::kConnected;
      connect_timer_channel_.disable_event();
      start();
      return;
    }
  };

  stream_channel_.update_event(weak_from_this(), core::EventType::kWrite,
                               core::EventMode::kDefault, std::move(socket_cb));

  /* 超时检查 */
  auto timer_cb = [this](const core::EventType) {
    /* 超时事件到->检查连接状态 */
    uint64_t val;
    while (read(connect_timer_channel_.get_fd(), &val, sizeof(val)) > 0)
      ;

    if (connected_ == ConnectState::kConnected) {
      /* 成功连接关闭定时器 */
      connect_timer_channel_.disable_event();
      return;
    }

    connected_ = ConnectState::kDisconnected;
    connect_timer_channel_.disable_event();
    on_error(ETIMEDOUT);
  };

  connect_timer_channel_.update_event(weak_from_this(), core::EventType::kRead,
                                      core::EventMode::kDefault,
                                      std::move(timer_cb));

  /* 启动超时器 */
  auto task = [this, timeout_ms]() {
    reset_timer_fd(connect_timer_channel_.get_fd(), timeout_ms);
  };

  connect_timer_channel_.post_io_task(weak_from_this(), std::move(task));
}

void StreamClientSocket::start() {
  if (bool expected{false}; !started_.compare_exchange_strong(expected, true)) {
    /* 已经启动 */
    return;
  }
  started_ = true;

  auto cb = [this](const core::EventType event_types) {
    bool disconnected{false};

    if ((event_types & core::EventType::kRead) == core::EventType::kRead) {
      /* 可读事件,循环读取缓冲区所有的流直到EAGIN */
      while (true) {
        switch (const auto status = stream_recv(stream_channel_, recv_buf_);
                status) {
        case IoStatus::kSuccess: {
          /* 读取到EGAIN */
          continue;
        }

        case IoStatus::kTryAgain: {
          break;
        }

        case IoStatus::kDisconnected: {
          /* 对端关闭 */
          disconnected = true;
          break;
        }

        case IoStatus::kError: {
          /* 系统错误，已打log */
          recv_buf_.clear();
          break;
        }

        default:
          /* 不可达代码 */
          assert(false && "unknown io status");
        }
        break;
      }
    }

    if ((event_types & core::EventType::kWrite) == core::EventType::kWrite) {
      /* 可写事件,循环写 send_buf_ 缓冲区的内容直到EAGIN */
      while (true) {
        switch (const auto status = stream_send(stream_channel_, send_buf_);
                status) {
        case IoStatus::kSuccess: {
          /* 发送成功 */
          if (send_buf_.empty()) {
            /* 缓冲区数据发送完毕 */
            /* 取消注册EPOLL可写事件 */
            const auto [old_types, old_modes] =
                stream_channel_.get_event_info();
            stream_channel_.update_event(old_types & ~core::EventType::kWrite,
                                         old_modes);
            break;
          }
          continue;
        }

        case IoStatus::kTryAgain: {
          break;
        }

        case IoStatus::kDisconnected: {
          /* 对端关闭 */
          disconnected = true;
          break;
        }

        case IoStatus::kError: {
          /* 系统错误，已打log */
          recv_buf_.clear();
          break;
        }

        default:
          /* 不可达代码 */
          assert(false && "unknown io status");
        }
        break;
      }
    }

    if ((event_types & core::EventType::kError) == core::EventType::kError) {
      /* 错误事件，清除 */
      int32_t err{0};
      socklen_t len = sizeof(err);
      getsockopt(stream_channel_.get_fd(), SOL_SOCKET, SO_ERROR, &err, &len);

      if (err != 0 && err != ECONNRESET && err != EPIPE) {
        if (err == ECONNRESET || err == EPIPE) {
          disconnected = true;
        } else {
          recv_buf_.clear();
          send_buf_.clear();
          on_error(err);
        }
      }
    }

    /* 其他事件暂不支持 */

    /* 处理缓冲区内所有数据 */
    while (true) {
      const auto consume_len = on_split();
      if (consume_len <= 0) {
        /* 缓冲区已经没有完整的一帧 */
        break;
      }

      /* 切分了一帧 */
      on_fragment({recv_buf_.begin(), recv_buf_.begin() + consume_len});

      /* 移除已处理的数据 */
      recv_buf_.pop(static_cast<ssize_t>(consume_len));
    }

    if (disconnected) {
      /* 对端关闭 */
      recv_buf_.clear();
      send_buf_.clear();
      on_disconnect();
    }
  };

  if (send_buf_.empty()) {
    /* 连接期间发送缓冲区无数据 */
    stream_channel_.update_event(
        weak_from_this(), core::EventType::kRead | core::EventType::kError,
        core::EventMode::kDefault, std::move(cb));
  } else {
    /* 连接期间发送缓冲区有数据 */
    stream_channel_.update_event(weak_from_this(),
                                 core::EventType::kRead |
                                     core::EventType::kWrite |
                                     core::EventType::kError,
                                 core::EventMode::kDefault, std::move(cb));
  }
}

void StreamClientSocket::stop() {
  started_ = false;
  stream_channel_.disable_event();
  connect_timer_channel_.disable_event();
}
} // namespace kzevent::net