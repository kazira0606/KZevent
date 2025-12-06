#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "kzevent/core.hpp"
#include "kzevent/log.hpp"
#include "kzevent/net/inet.hpp"
#include "kzevent/sys_error.hpp"

namespace kzevent::net {
/*-------------------- 网络地址  --------------------*/
bool InetAddr::operator==(const InetAddr &other) const {
  if (addr_storage_.ss_family != other.addr_storage_.ss_family) {
    return false;
  }

  switch (addr_storage_.ss_family) {
  case AF_INET: {
    auto self_ipv4_in{reinterpret_cast<const sockaddr_in *>(&addr_storage_)};
    auto other_ipv4_in{
        reinterpret_cast<const sockaddr_in *>(&other.addr_storage_)};

    /* 比较port和ip */
    return self_ipv4_in->sin_port == other_ipv4_in->sin_port &&
           self_ipv4_in->sin_addr.s_addr == other_ipv4_in->sin_addr.s_addr;
  }

  case AF_INET6: {
    auto self_ipv6_in{reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};
    auto other_ipv6_in{
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

    auto self_un_in{reinterpret_cast<const sockaddr_un *>(&addr_storage_)};
    auto other_un_in{
        reinterpret_cast<const sockaddr_un *>(&other.addr_storage_)};

    const auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
    if (socklen_ <= base) {
      return false;
    }
    const auto sun_len{static_cast<size_t>(socklen_ - base)};

    /* 比较sun_path */
    return std::memcmp(self_un_in->sun_path, other_un_in->sun_path, sun_len) ==
           0;
  }

  default: {
    assert(false && "invalid addr type");
    return false;
  }
  }
}

/* 静态工厂 */
std::optional<InetAddr> InetAddr::make_ipv4(const std::string &ip,
                                            uint16_t port) {
  try {
    return InetAddr{ip, port, AF_INET, 0};
  } catch (std::invalid_argument &e) {
    KZ_LOG_ERROR("invalid ipv4 address");
    return std::nullopt;
  } catch (const std::exception &e) {
    sys_error::fatal(e);
  }
}

std::optional<InetAddr> InetAddr::make_ipv6(const std::string &ip,
                                            uint16_t port, uint32_t scope_id) {
  try {
    return InetAddr{ip, port, AF_INET6, scope_id};
  } catch (std::invalid_argument &e) {
    KZ_LOG_ERROR("invalid ipv6 address");
    return std::nullopt;
  } catch (std::exception &e) {
    sys_error::fatal(e);
  }
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

  try {
    return InetAddr{path, false};
  } catch (std::invalid_argument &e) {
    KZ_LOG_ERROR("invalid unix socket name/path");
    return std::nullopt;
  } catch (const std::exception &e) {
    sys_error::fatal(e);
  }
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

  try {
    return InetAddr{name, true};
  } catch (std::invalid_argument &e) {
    KZ_LOG_ERROR("invalid unix socket name/path");
    return std::nullopt;
  } catch (const std::exception &e) {
    sys_error::fatal(e);
  }
}

std::optional<InetAddr> InetAddr::make_from_sockaddr(const sockaddr *addr,
                                                     socklen_t len) {
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
    try {
      return InetAddr{storage, sizeof(sockaddr_in)};
    } catch (std::invalid_argument &e) {
      KZ_LOG_ERROR("invalid ipv4 address");
      return std::nullopt;
    } catch (const std::exception &e) {
      sys_error::fatal(e);
    }
  }

  case AF_INET6: {
    if (len < static_cast<socklen_t>(sizeof(sockaddr_in6))) {
      KZ_LOG_ERROR("invalid sockaddr_in6 length");
      return std::nullopt;
    }

    /* ipv6反序列化 */
    sockaddr_storage storage{};
    std::memcpy(&storage, addr, sizeof(sockaddr_in6));
    try {
      return InetAddr{storage, sizeof(sockaddr_in6)};
    } catch (std::invalid_argument &e) {
      KZ_LOG_ERROR("invalid ipv6 address");
      return std::nullopt;
    } catch (const std::exception &e) {
      sys_error::fatal(e);
    }
  }

  case AF_UNIX: {
    const auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
    if (len <= base || len > static_cast<socklen_t>(sizeof(sockaddr_un))) {
      /* 不允许无名/过长 */
      KZ_LOG_ERROR("unix socket name/path is empty or too long");
      return std::nullopt;
    }

    const auto sun_len{static_cast<size_t>(len - base)};

    auto un_in{reinterpret_cast<const sockaddr_un *>(addr)};
    /* unix反序列化 */
    if (un_in->sun_path[0] == '\0') {
      /* 抽象型 */
      auto name{std::string{un_in->sun_path + 1, sun_len - 1}};

      while (!name.empty() && name.back() == '\0') {
        /* 去除冗余'\0' */
        name.pop_back();
      }

      return make_abstract(name);
    } else {
      /* 路径型 */
      auto path{std::string{un_in->sun_path, sun_len}};

      while (!path.empty() && path.back() == '\0') {
        /* 去除冗余'\0' */
        path.pop_back();
      }

      return make_unix(path);
    }
  }

  default: {
    KZ_LOG_ERROR("invalid address family");
    return std::nullopt;
  }
  }
}

/* sys交互接口 */
const sockaddr *InetAddr::get_sockaddr() const {
  return reinterpret_cast<const sockaddr *>(&addr_storage_);
}

socklen_t InetAddr::get_socklen() const { return socklen_; }

/* 信息接口 */
std::string InetAddr::get_ip_or_path() const {
  switch (addr_storage_.ss_family) {
  case AF_INET: {
    /* ipv4 */
    auto ipv4_in{reinterpret_cast<const sockaddr_in *>(&addr_storage_)};

    std::string ip_buf(INET_ADDRSTRLEN, '\0');
    if (const auto ret = inet_ntop(AF_INET, &ipv4_in->sin_addr, ip_buf.data(),
                                   ip_buf.size());
        ret == nullptr) {
      sys_error::error();
    }

    /* 去除冗余'\0' */
    ip_buf.resize(std::strlen(ip_buf.c_str()));
    return ip_buf;
  }

  case AF_INET6: {
    /* ipv6 */
    auto ipv6_in{reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};

    std::string ip_buf(INET6_ADDRSTRLEN, '\0');
    if (const auto ret = inet_ntop(AF_INET6, &ipv6_in->sin6_addr, ip_buf.data(),
                                   ip_buf.size());
        ret == nullptr) {
      sys_error::error();
    }

    /* 去除冗余'\0' */
    ip_buf.resize(std::strlen(ip_buf.c_str()));
    return ip_buf;
  }

  case AF_UNIX: {
    auto un_in{reinterpret_cast<const sockaddr_un *>(&addr_storage_)};

    const auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};
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
    assert(false && "invalid addr type");
    return {};
  }
}

uint16_t InetAddr::get_port() const {
  switch (addr_storage_.ss_family) {
  case AF_INET: {
    auto ipv4_in{reinterpret_cast<const sockaddr_in *>(&addr_storage_)};
    return ntohs(ipv4_in->sin_port);
  }

  case AF_INET6: {
    auto ipv6_in{reinterpret_cast<const sockaddr_in6 *>(&addr_storage_)};
    return ntohs(ipv6_in->sin6_port);
  }

  case AF_UNIX: {
    /* unix socket */
    return 0;
  }

  default: {
    assert(false && "invalid addr type");
    return 0;
  }
  }
}

/* 构造函数 */
InetAddr::InetAddr(const std::string &ip, uint16_t port, sa_family_t type,
                   uint32_t scope_id) {
  switch (type) {
  case AF_INET: {
    const auto ipv4_in{reinterpret_cast<sockaddr_in *>(&addr_storage_)};

    if (const auto ret = inet_pton(AF_INET, ip.c_str(), &ipv4_in->sin_addr);
        ret == 0) {
      /* 0 无效ip */
      KZ_LOG_ERROR("invalid ipv4 address: ", ip.c_str());
      throw std::invalid_argument("invalid ipv4 address");
    } else if (ret == -1) {
      /* -1 系统错误 */
      sys_error::fatal();
    }

    /* ipv4 */
    ipv4_in->sin_family = AF_INET;
    ipv4_in->sin_port = htons(port);
    socklen_ = sizeof(sockaddr_in);

    return;
  }

  case AF_INET6: {
    const auto ipv6_in{reinterpret_cast<sockaddr_in6 *>(&addr_storage_)};

    if (const auto ret = inet_pton(AF_INET6, ip.c_str(), &ipv6_in->sin6_addr);
        ret == 0) {
      /* 0 无效ip */
      KZ_LOG_ERROR("invalid ipv6 address: ", ip.c_str());
      throw std::invalid_argument("invalid ipv6 address");
    } else if (ret == -1) {
      /* -1 系统错误 */
      sys_error::fatal();
    }

    /* ipv6 */
    ipv6_in->sin6_family = AF_INET6;
    ipv6_in->sin6_port = htons(port);
    ipv6_in->sin6_scope_id = scope_id;
    socklen_ = sizeof(sockaddr_in6);

    return;
  }

  default: {
    assert(false && "invalid addr type");
    return;
  }
  }
};

InetAddr::InetAddr(sockaddr_storage addr_storage, socklen_t socklen) {
  addr_storage_ = addr_storage;
  socklen_ = socklen;
}

InetAddr::InetAddr(const std::string &path, bool abstract) {
  const auto un_in{reinterpret_cast<sockaddr_un *>(&addr_storage_)};
  un_in->sun_family = AF_UNIX;

  const auto base{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path))};

  /* 字符串长度过长 */
  if (path.size() + 1 > sizeof(un_in->sun_path)) {
    KZ_LOG_ERROR("unix socket path/name too long: ", path.c_str());
    throw std::invalid_argument("unix socket path/name too long");
  }

  if (abstract) {
    /* 抽象型开头'\0'占位，结尾无需'\0' */
    un_in->sun_path[0] = '\0';
    path.copy(un_in->sun_path + 1, path.size());
    socklen_ = static_cast<socklen_t>(base + 1 + path.size());
  } else {
    /* 文件路径型，c风格字符串 */
    path.copy(un_in->sun_path, path.size());
    un_in->sun_path[path.size()] = '\0';
    socklen_ = static_cast<socklen_t>(base + path.size() + 1);
  }
}

/*-------------------- 网络工厂  --------------------*/
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

  if (const auto ret = bind(fd, addr.get_sockaddr(), addr.get_socklen());
      ret == -1) {
    sys_error::error();
    return std::nullopt;
  }

  return udp_channel;
}
} // namespace kzevent::net