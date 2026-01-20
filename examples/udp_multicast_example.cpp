#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

int main() {
  using namespace kzevent::core;
  using namespace kzevent::net;
  using namespace kzevent::net::udp;

  /* 创建一个loop */
  auto loop = std::make_unique<Loop>();
  loop->start();

  /* 创建一个多播组 */
  auto multicast_group = InetAddr::make_ipv4("239.255.255.250", 9999).value();

  /* 创建一个监听频道 */
  auto local_listen = InetAddr::make_ipv4("0.0.0.0", 9999).value();
  auto node = UdpNode::make_udp_node(*loop, local_listen);

  /* 加入多播组 */
  node->joint_group(multicast_group);

  /* 设置接收回调*/
  node->set_read_cb([](const std::shared_ptr<UdpNode> &udp_node,
                       std::vector<uint8_t> data, InetAddr source) {
    std::string msg(data.begin(), data.end());
    std::cout << "[multicast] from: " << source.get_ip_or_path()
              << " msg: " << msg << std::endl;
  });

  /* 循环多播 */
  int32_t count = 0;
  while (true) {
    std::string heart_beat = "still alive: " + std::to_string(count++);
    std::vector<uint8_t> payload(heart_beat.begin(), heart_beat.end());

    /* 发送多播消息 */
    node->post_send_task(std::move(payload), multicast_group);

    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  return 0;
}
