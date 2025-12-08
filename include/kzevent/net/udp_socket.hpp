#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kzevent/core.hpp"
#include "kzevent/net/inet.hpp"

namespace kzevent::net::udp {
class UdpNode;

/* 回调类型 */
using ReadCallBack =
    std::function<void(const std::shared_ptr<UdpNode> &udp_node,
                       std::vector<std::uint8_t> &data, InetAddr source)>;
using ErrorCallBack = std::function<void()>;

/* udp类型  */
enum class UdpType { kUdpIpv4, kUdpIpv6, kUnix, kAbstract };

/*-------------------- UDP node  --------------------*/
class UdpNode : public std::enable_shared_from_this<UdpNode> {
public:
  ~UdpNode() = default;

  /* 静态工厂，只允许shared_ptr管理 */
  static std::shared_ptr<UdpNode> make_by_any(core::Loop &loop, UdpType type,
                                              uint16_t port);

  static std::shared_ptr<UdpNode> make_by_ip(core::Loop &loop, UdpType type,
                                             const std::string &ip,
                                             uint16_t port);

  static std::shared_ptr<UdpNode> make_by_path(core::Loop &loop, UdpType type,
                                               const std::string &path);

  /* 接口 */

  template <typename container>
  void post_send_task(container data, InetAddr addr);

  template <typename Fun> void post_heavy_task(Fun task);

  void set_recv_cb(ReadCallBack cb) noexcept;

  void set_error_cb(ErrorCallBack cb) noexcept;

  void start();

  void stop();

private:
  UdpNode(core::Loop &loop, const InetAddr &addr);

  core::LoopChannel udp_channel_;
  std::vector<uint8_t> recv_buf_{};

  /* 回调 */
  ReadCallBack read_cb_{};
  ErrorCallBack error_cb_{};
};

/*-------------------- UDP session  --------------------*/

/*-------------------- 模板实现  --------------------*/
template <typename container>
void UdpNode::post_send_task(container data, InetAddr addr) {
  auto task = [this, data = std::move(data), addr]() {
    udp_send(udp_channel_, data, addr);
  };

  udp_channel_.post_io_task(weak_from_this(), std::move(task));
}

template <typename Fun> void UdpNode::post_heavy_task(Fun task) {
  udp_channel_.post_heavy_task(weak_from_this(), std::move(task));
}
} // namespace kzevent::net::udp