
#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/udp_socket.hpp"

using namespace kzevent;

/* 测试类 */
class KZUdpTest : public testing::Test {
protected:
  void SetUp() override {
    loop_ = std::make_unique<core::Loop>();
    loop_->start();
  }

  void TearDown() override {
    loop_->stop();
    loop_.reset();
  }

  std::unique_ptr<core::Loop> loop_{};
};

/*-------------------- UdpNode + UdpClient ipv4测试 --------------------*/
TEST_F(KZUdpTest, EchoUdpNodeTestIpv4) {
  using namespace kzevent::net::udp;
  constexpr uint16_t server_port{6666};
  constexpr uint16_t client_port_start{6667};

  /* 发送数据 */
  const std::vector<uint8_t> enco_data{'H', 'e', 'l', 'l', 'o'};

  /* 预期回应数据 */
  std::vector<uint8_t> expected_data{enco_data};
  expected_data.push_back(enco_data.size());

  /* 回声node节点 */
  const auto echo_node_addr = net::InetAddr::make_ipv4("0.0.0.0", server_port);
  ASSERT_TRUE(echo_node_addr.has_value());

  const auto echo_node = UdpNode::make_udp_node(*loop_, echo_node_addr.value());
  ASSERT_TRUE(echo_node);

  /* 设置node回调 */
  echo_node->set_read_cb(
      [](const auto &udp_node, auto data, auto source) mutable {
        /* heavy线程处理数据 */
        auto heavy_task = [udp_node, source, data = std::move(data)]() mutable {
          /* 处理数据 */
          data.push_back(data.size());
          /* 回到io线程发送数据 */
          udp_node->post_send_task(std::move(data), source);
        };

        udp_node->post_heavy_task(std::move(heavy_task));
      });

  /* 启动服务节点 */
  echo_node->start();

  /* 统计收到的包数量 */
  std::atomic<int> success_count{0};

  /* 创建KZEVENT_CLIENT_NUM个客户端 */
  std::vector<std::thread> client_threads{};

  for (size_t client_index{0}; client_index < KZEVENT_CLIENT_NUM;
       ++client_index) {
    client_threads.emplace_back([&, client_index]() {
      /* 创建客户端 */
      const auto client_addr =
          net::InetAddr::make_ipv4("0.0.0.0", client_port_start + client_index);
      ASSERT_TRUE(client_addr.has_value());

      const auto source_addr =
          net::InetAddr::make_ipv4("127.0.0.1", server_port);
      ASSERT_TRUE(source_addr.has_value());

      const auto client = UdpClient::make_udp_client(
          *loop_, client_addr.value(), source_addr.value());
      ASSERT_TRUE(client);

      /* 使用promise确保回调完成 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 设置客户端回调 */
      client->set_read_cb([&success_count, &expected_data,
                           done](const auto &udp_client, auto data) {
        /* 校验数据 */
        EXPECT_EQ(data, expected_data);
        /* 统计正确收到的包 */
        success_count.fetch_add(1);
        /* 接收完毕 */
        done->set_value();
      });

      /* 启动客户端 */
      client->start();

      /* 客户端发送数据 */
      client->post_send_task(enco_data);

      /* 等待回调执行 */
      if (future.wait_for(std::chrono::seconds(1)) !=
          std::future_status::ready) {
        /* 超时 */
        ADD_FAILURE() << "client " << client_index
                      << " timeout waiting for response";
      }
    });
  }

  /* 等待所有客户端线程完成 */
  for (auto &t : client_threads) {
    t.join();
  }

  /* 验证成功数量 */
  EXPECT_EQ(success_count.load(), KZEVENT_CLIENT_NUM);
}

/*-------------------- UdpNode和UdpClient stop测试 --------------------*/
TEST_F(KZUdpTest, UdpNodeStopTest) {
  using namespace net::udp;
  constexpr uint16_t server_port{8888};

  const std::vector<uint8_t> data{'H', 'i'};

  /* 创建并启动 enco node */
  const auto enco_node_addr = net::InetAddr::make_ipv4("0.0.0.0", server_port);
  ASSERT_TRUE(enco_node_addr.has_value());

  const auto enco_node = UdpNode::make_udp_node(*loop_, enco_node_addr.value());
  ASSERT_TRUE(enco_node);

  std::atomic<int> recv_count{0};

  /* 回调保证 */
  auto first_packet_done = std::make_shared<std::promise<void>>();
  auto future = first_packet_done->get_future();

  enco_node->set_read_cb([&recv_count, first_packet_done](
                             const auto &udp_node, auto data, auto source) {
    recv_count.fetch_add(1);

    /* 只有第一次收到包时，才通知测试主线程 */
    if (recv_count.load() == 1) {
      first_packet_done->set_value();
    }
  });

  enco_node->start();

  /* 创建 Client */
  const auto client_addr = net::InetAddr::make_ipv4("0.0.0.0", 0);
  ASSERT_TRUE(client_addr.has_value());

  const auto server_addr = net::InetAddr::make_ipv4("127.0.0.1", server_port);
  ASSERT_TRUE(server_addr.has_value());

  const auto client = UdpClient::make_udp_client(*loop_, client_addr.value(),
                                                 server_addr.value());
  ASSERT_TRUE(client);

  client->start();

  /* 第一阶段：正常发送一个包 */
  client->post_send_task(data);

  /* 等待 Server 收到第一个包的回应 */
  ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(recv_count.load(), 1);

  /* 第二阶段：调用 Stop */
  enco_node->stop();

  /* 再次发送 */
  client->post_send_task(data);
  client->post_send_task(data);

  /* 等待并验证 */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /* stop生效，只收到了stop之前的包 */
  EXPECT_EQ(recv_count.load(), 1);
}

/*-------------------- UdpServer和UdpClient ipv6测试 --------------------*/
TEST_F(KZUdpTest, EchoUdpServerTestIpv6_ContextFeature) {
  using namespace kzevent::net::udp;
  constexpr uint16_t server_port{7777};
  constexpr uint16_t client_port_start{7778};

  /* 回声server节点 */
  const auto echo_server_addr = net::InetAddr::make_ipv6("::", server_port);
  ASSERT_TRUE(echo_server_addr.has_value());

  const auto echo_server =
      UdpServer::make_udp_server(*loop_, echo_server_addr.value());
  ASSERT_TRUE(echo_server);

  /* 定义协议常量 */
  const std::vector<uint8_t> cmd_ack{'A', 'C', 'K'};
  const std::vector<uint8_t> cmd_get{'G', 'E', 'T'};

  /* 设置新会话回调 (存储数据到Context) */
  echo_server->set_new_session_cb([&](const auto &session, auto data) mutable {
    /* 首次连接，将用户数据保存到 Session Context 中 */
    auto context_data = std::make_shared<std::vector<uint8_t>>(std::move(data));
    session->set_user_context(context_data);

    /* 回复 ACK 确认存储完毕 */
    session->post_send_task(cmd_ack);
  });

  /* 设置已有会话回调 (老连接逻辑：响应 GET 指令) */
  echo_server->set_read_cb([&](const auto &session, auto data) mutable {
    /* 校验指令是否为 GET */
    if (data == cmd_get) {
      /* 从 Context 中取出之前存储的数据 */
      auto context_ptr = session->get_user_context();

      /* 转换回原数据类型 */
      auto stored_data_ptr =
          std::static_pointer_cast<std::vector<uint8_t>>(context_ptr);

      /* 模拟业务处理：追加大小 */
      /* 是否不拷贝而是直接修改contx则由业务逻辑决定，此处拷贝 */
      std::vector<uint8_t> response_data = *stored_data_ptr;
      response_data.push_back(response_data.size());

      /* 发送处理后的数据 */
      session->post_send_task(std::move(response_data));
    } else {
      /* 未知指令*/
      FAIL() << "unknown command";
    }
  });

  /* 启动服务节点 */
  echo_server->start();

  /* 统计完全完成交互的客户端数量 */
  std::atomic<int> success_count{0};

  /* 创建KZEVENT_CLIENT_NUM个客户端 */
  std::vector<std::thread> client_threads{};

  for (size_t client_index{0}; client_index < KZEVENT_CLIENT_NUM;
       ++client_index) {
    client_threads.emplace_back([&, client_index]() {
      /* 创建客户端 */
      const auto client_addr =
          net::InetAddr::make_ipv6("::", client_port_start + client_index);
      ASSERT_TRUE(client_addr.has_value());

      /* 目标地址 */
      const auto source_addr = net::InetAddr::make_ipv6("::1", server_port);
      ASSERT_TRUE(source_addr.has_value());

      const auto client = UdpClient::make_udp_client(
          *loop_, client_addr.value(), source_addr.value());
      ASSERT_TRUE(client);

      /*构造独一无二的数据 (测试Session路由) */
      std::string str_unique = "Secret_Data_" + std::to_string(client_index);
      const std::vector<uint8_t> unique_data(str_unique.begin(),
                                             str_unique.end());

      /* 预期的最终结果：原始数据 + Size */
      std::vector<uint8_t> expected_final_data = unique_data;
      expected_final_data.push_back(unique_data.size());

      /* 使用promise确保流程结束 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 状态标记：是否收到过ACK */
      auto has_received_ack = std::make_shared<bool>(false);

      /* 设置客户端回调 */
      client->set_read_cb(
          [&success_count, unique_data, cmd_ack, cmd_get, expected_final_data,
           done, has_received_ack](const auto &udp_client, auto data) mutable {
            if (!(*has_received_ack)) {
              /* 阶段一=：收到 ACK */
              EXPECT_EQ(data, cmd_ack);

              /* 标记阶段完成 */
              *has_received_ack = true;

              /* 进入阶段二：发送 GET 指令，索要数据 */
              udp_client->post_send_task(cmd_get);
            } else {
              /* --- 阶段二验证：收到处理后的 Context 数据 --- */
              EXPECT_EQ(data, expected_final_data);

              /* 流程全部结束 */
              success_count.fetch_add(1);
              done->set_value();
            }
          });

      /* 启动客户端 */
      client->start();

      /* 触发阶段一：发送独一无二的数据 */
      client->post_send_task(unique_data);

      /* 等待完整流程结束 */
      if (future.wait_for(std::chrono::seconds(2)) !=
          std::future_status::ready) {
        /* 超时 */
        ADD_FAILURE() << "client " << client_index
                      << " timeout waiting for interaction sequence";
      }
    });
  }

  /* 等待所有客户端线程完成 */
  for (auto &t : client_threads) {
    t.join();
  }

  /* 验证成功数量 */
  EXPECT_EQ(success_count.load(), KZEVENT_CLIENT_NUM);
}