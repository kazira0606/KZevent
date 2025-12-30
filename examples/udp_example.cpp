#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

/* 传输协议 */
const std::vector<uint8_t> GET{'G', 'E', 'T'};

int main() {
  using namespace kzevent::core;
  using namespace kzevent::net;
  using namespace kzevent::net::udp;

  /* 创建一个loop */
  auto loop = std::make_unique<Loop>();
  loop->start();

  /*-------------------- server --------------------*/

  /* 创建一个udp服务 */
  auto server_addr = InetAddr::make_ipv4("127.0.0.1", 6666).value();
  auto server = UdpServer::make_udp_server(*loop, server_addr);

  /* 设置新连接回调（非必须项，不设置会调用数据接收回调） */
  auto new_session_cb = [](const UdpServer::Session &session,
                           std::vector<uint8_t> data) {
    /* 给新连接创建上下文 */
    session->set_user_context(std::make_shared<std::vector<uint8_t>>());

    /* 将接收数据加入上下文 */
    auto ctx = std::static_pointer_cast<std::vector<uint8_t>>(
        session->get_user_context());
    ctx->assign(data.begin(), data.end());

    /* 回声 */
    session->post_send_task(std::vector<uint8_t>{'N', 'E', 'W'});
  };

  /* 设置数据接收回调（必须项） */
  auto read_cb = [](const UdpServer::Session &session,
                    std::vector<uint8_t> data) {
    /* 获取上下文 */
    auto ctx = std::static_pointer_cast<std::vector<uint8_t>>(
        session->get_user_context());

    if (data != GET) {
      /* 更新上下文 */
      ctx->assign(data.begin(), data.end());
    } else {
      /* 回声 */
      session->post_send_task(*ctx);
    }
  };

  server->set_read_cb(read_cb);
  server->set_new_session_cb(new_session_cb);

  /*-------------------- client --------------------*/

  /* 创建一个客户端 */
  auto client_addr = InetAddr::make_ipv4("127.0.0.1", 6667).value();
  auto client = UdpClient::make_udp_client(*loop, client_addr, server_addr);

  client->set_read_cb([](const std::shared_ptr<UdpClient> &udp_client,
                         std::vector<uint8_t> data) {
    /* 收到数据就打印 */
    std::cout << std::string(data.begin(), data.end()) << std::endl;
  });

  /* 首次发送 */
  client->post_send_task(std::vector<uint8_t>{'A', 'A', 'A'});
  while (true) {
    /* 持续GET */
    client->post_send_task(GET);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}