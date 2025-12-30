#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/unix_socket.hpp"

/* 终止信号 */
static std::atomic<bool> run{true};

void signal_handler(int sig) { run = false; }

/* 定义一个简单帧协议 */
constexpr uint32_t FRAM_HEAD = 4;
std::vector<uint8_t> data_to_packet(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> packet;
  packet.reserve(FRAM_HEAD + data.size());
  uint32_t len = data.size();
  std::copy(reinterpret_cast<uint8_t *>(&len),
            reinterpret_cast<uint8_t *>(&len) + sizeof(len),
            std::back_inserter(packet));
  packet.insert(packet.end(), data.begin(), data.end());
  return packet;
}

/* 传输协议 */
const std::vector<uint8_t> GET{'G', 'E', 'T'};

int main() {
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  using namespace kzevent::core;
  using namespace kzevent::net;
  using namespace kzevent::net::unix;

  /* 创建一个loop */
  auto loop = std::make_unique<Loop>();
  loop->start();

  /*-------------------- server --------------------*/

  /* 创建一个unix服务(abstract类型) */
  auto server_addr = InetAddr::make_abstract("example_server").value();
  auto server = UnixServer::make_unix_server(*loop, server_addr);

  /* 切包回调 */
  auto server_split_cb = [](const UnixServer::Session &,
                            std::string_view slice) -> ssize_t {
    /* 成功拆包返回包长，失败返回0 */
    if (slice.size() < 4)
      return 0;
    uint32_t len{};
    std::copy(slice.data(), slice.data() + 4,
              reinterpret_cast<uint8_t *>(&len));
    return (len + 4 <= slice.size()) ? (len + 4) : 0;
  };

  /* 服务端帧回调 */
  auto server_fragment_cb = [](const UnixServer::Session &session,
                               std::vector<uint8_t> fragment) {
    /* 获取上下文 */
    auto raw_ctx = session->get_user_context();

    if (raw_ctx == nullptr) {
      /* 新连接 */
      /* 创建上下文 */
      auto new_ctx = std::make_shared<std::vector<uint8_t>>();
      new_ctx->assign(fragment.begin() + 4, fragment.end());
      session->set_user_context(new_ctx);

      /* 回复 NEW 确认 */
      std::vector<uint8_t> resp{'N', 'E', 'W'};
      session->post_send_task(data_to_packet(resp));
    } else {
      /* 已有连接 */
      /* 获取上下文 */
      auto ctx = std::static_pointer_cast<std::vector<uint8_t>>(raw_ctx);

      if (fragment == data_to_packet(GET)) {
        /* 回声发送上下文存储的数据 */
        session->post_send_task(data_to_packet(*ctx));
      } else {
        /* 更新上下文 */
        ctx->assign(fragment.begin() + FRAM_HEAD, fragment.end());
      }
    }
  };

  server->set_split_cb(server_split_cb);
  server->set_fragment_cb(server_fragment_cb);

  /* 设置断连回调 */
  server->set_disconnect_cb([](const auto &) {
    std::cout << "[Server] Client disconnected, session destroyed."
              << std::endl;
  });

  /*-------------------- client --------------------*/

  /* 创建一个 TCP 客户端(path 类型) */
  auto client_addr =
      InetAddr::make_unix("/tmp/unix_example_client.sock").value();
  auto client = UnixClient::make_unix_client(*loop, client_addr);

  /* 客户端拆包逻辑：同服务端 */
  auto client_split_cb = [](const std::shared_ptr<UnixClient> &,
                            std::string_view slice) -> ssize_t {
    /* 成功拆包返回包长，失败返回0 */
    if (slice.size() < 4)
      return 0;
    uint32_t len{};
    std::copy(slice.data(), slice.data() + 4,
              reinterpret_cast<uint8_t *>(&len));
    return (len + 4 <= slice.size()) ? (len + 4) : 0;
  };

  /* 客户端帧回调 */
  auto client_fragment_cb = [](const std::shared_ptr<UnixClient> &session,
                               std::vector<uint8_t> fragment) {
    std::cout << "[Client] Received: "
              << std::string(fragment.begin() + FRAM_HEAD, fragment.end())
              << std::endl;
  };

  client->set_split_cb(client_split_cb);
  client->set_fragment_cb(client_fragment_cb);

  /* 连接服务器 */
  client->connect(server_addr, 1000);

  /* 首次发送：初始化数据 */
  client->post_send_task(data_to_packet(std::vector<uint8_t>{'A', 'A', 'A'}));

  while (run) {
    /* 持续 GET */
    client->post_send_task(data_to_packet(GET));
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}
