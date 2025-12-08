
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
class KZCoreTest : public testing::Test {
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

/*-------------------- UdpNode ipv4测试 --------------------*/
TEST_F(KZCoreTest, EchoUdpNodeTestIpv4) {
  using namespace net::udp;
  constexpr uint16_t server_port{6666};
  constexpr uint16_t client_port_start{6667};

  /* 发送数据 */
  const std::vector<uint8_t> enco_data{'H', 'e', 'l', 'l', 'o'};

  /* 预期回应数据 */
  std::vector<uint8_t> expected_data{enco_data};
  expected_data.push_back(enco_data.size());

  /* 回声服务节点 */
  const auto echo_node =
      UdpNode::make_by_any(*loop_, UdpType::kUdpIpv4, server_port);
  ASSERT_TRUE(echo_node);

  /* 设置服务器回调 */
  echo_node->set_recv_cb([](const std::shared_ptr<UdpNode> &udp_node,
                            std::vector<uint8_t> &data,
                            const net::InetAddr &source) {
    /* heavy线程处理数据 */
    auto task = [udp_node, data = std::move(data), source]() mutable {
      /* 处理数据 */
      data.push_back(data.size());

      /* 回到io线程发送数据 */
      udp_node->post_send_task(std::move(data), source);
    };

    udp_node->post_heavy_task(std::move(task));
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
      const auto client = UdpNode::make_by_any(
          *loop_, UdpType::kUdpIpv4, client_port_start + client_index);
      ASSERT_TRUE(client);

      /* 使用promise确保回调完成 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 设置客户端回调 */
      client->set_recv_cb([&success_count, &expected_data,
                           &done](const std::shared_ptr<UdpNode> &udp_node,
                                  std::vector<uint8_t> &data,
                                  const net::InetAddr &source) {
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
      const auto server_addr =
          net::InetAddr::make_ipv4("127.0.0.1", server_port);
      ASSERT_TRUE(server_addr.has_value());

      client->post_send_task(enco_data, server_addr.value());

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

/*-------------------- UdpNode ipv6回声测试 --------------------*/
TEST_F(KZCoreTest, EchoUdpNodeTestIpv6) {
  using namespace net::udp;
  constexpr uint16_t server_port{6666};
  constexpr uint16_t client_port_start{6667};

  /* 发送数据 */
  const std::vector<uint8_t> enco_data{'H', 'e', 'l', 'l', 'o'};

  /* 预期回应数据 */
  std::vector<uint8_t> expected_data{enco_data};
  expected_data.push_back(enco_data.size());

  /* 回声服务节点 */
  const auto echo_node =
      UdpNode::make_by_any(*loop_, UdpType::kUdpIpv6, server_port);
  ASSERT_TRUE(echo_node);

  /* 设置服务器回调 */
  echo_node->set_recv_cb([](const std::shared_ptr<UdpNode> &udp_node,
                            std::vector<uint8_t> &data,
                            const net::InetAddr &source) {
    /* heavy线程处理数据 */
    auto task = [udp_node, data = std::move(data), source]() mutable {
      /* 处理数据 */
      data.push_back(data.size());

      /* 回到io线程发送数据 */
      udp_node->post_send_task(std::move(data), source);
    };

    udp_node->post_heavy_task(std::move(task));
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
      const auto client = UdpNode::make_by_any(
          *loop_, UdpType::kUdpIpv6, client_port_start + client_index);
      ASSERT_TRUE(client);

      /* 使用promise确保回调完成 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 设置客户端回调 */
      client->set_recv_cb([&success_count, &expected_data,
                           &done](const std::shared_ptr<UdpNode> &udp_node,
                                  std::vector<uint8_t> &data,
                                  const net::InetAddr &source) {
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
      const auto server_addr = net::InetAddr::make_ipv6("::1", server_port);
      ASSERT_TRUE(server_addr.has_value());

      client->post_send_task(enco_data, server_addr.value());

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

/*-------------------- UdpNode Unix套接字回声测试 --------------------*/
TEST_F(KZCoreTest, EchoUdpNodeTestUnix) {
  using namespace net::udp;
  const std::string unix_socket_path_base{"/tmp/echo_test_socket"};

  /* 发送数据 */
  const std::vector<uint8_t> enco_data{'H', 'e', 'l', 'l', 'o'};

  /* 预期回应数据 */
  std::vector<uint8_t> expected_data{enco_data};
  expected_data.push_back(enco_data.size());

  /* 回声服务节点 */
  const auto echo_node = UdpNode::make_by_path(
      *loop_, UdpType::kUnix, unix_socket_path_base + "_server");
  ASSERT_TRUE(echo_node);

  /* 设置服务器回调 */
  echo_node->set_recv_cb([](const std::shared_ptr<UdpNode> &udp_node,
                            std::vector<uint8_t> &data,
                            const net::InetAddr &source) {
    /* heavy线程处理数据 */
    auto task = [udp_node, data = std::move(data), source]() mutable {
      /* 处理数据 */
      data.push_back(data.size());

      /* 回到io线程发送数据 */
      udp_node->post_send_task(std::move(data), source);
    };

    udp_node->post_heavy_task(std::move(task));
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
      /* 创建唯一的 Unix 套接字路径 */
      const std::string client_socket_path{unix_socket_path_base + "_client_" +
                                           std::to_string(client_index)};

      /* 创建客户端 */
      const auto client =
          UdpNode::make_by_path(*loop_, UdpType::kUnix, client_socket_path);
      ASSERT_TRUE(client);

      /* 使用promise确保回调完成 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 设置客户端回调 */
      client->set_recv_cb([&success_count, &expected_data,
                           &done](const std::shared_ptr<UdpNode> &udp_node,
                                  std::vector<uint8_t> &data,
                                  const net::InetAddr &source) {
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
      const auto server_addr =
          net::InetAddr::make_unix(unix_socket_path_base + "_server");
      ASSERT_TRUE(server_addr.has_value());

      client->post_send_task(enco_data, server_addr.value());

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

  /* 测试完成后，手动删除所有的 Unix 套接字文件 */
  if (unlink((unix_socket_path_base + "_server").c_str()) == -1) {
    ADD_FAILURE() << "failed to unlink unix socket file (server): "
                  << unix_socket_path_base + "_server";
  }

  for (size_t client_index{0}; client_index < KZEVENT_CLIENT_NUM;
       ++client_index) {
    std::string client_socket_path{unix_socket_path_base + "_client_" +
                                   std::to_string(client_index)};

    if (unlink(client_socket_path.c_str()) == -1) {
      ADD_FAILURE() << "failed to unlink unix socket file (client "
                    << client_index << "): " << client_socket_path;
    }
  }

  /* 验证成功数量 */
  EXPECT_EQ(success_count.load(), KZEVENT_CLIENT_NUM);
}

/*-------------------- UdpNode Abstract套接字回声测试 --------------------*/
TEST_F(KZCoreTest, EchoUdpNodeTestAbstract) {
  using namespace net::udp;
  const std::string abstract_socket_path_base{"test_socket"};

  /* 发送数据 */
  const std::vector<uint8_t> enco_data{'H', 'e', 'l', 'l', 'o'};

  /* 预期回应数据 */
  std::vector<uint8_t> expected_data{enco_data};
  expected_data.push_back(enco_data.size());

  /* 回声服务节点 */
  const auto echo_node = UdpNode::make_by_path(
      *loop_, UdpType::kAbstract, abstract_socket_path_base + "_server");
  ASSERT_TRUE(echo_node);

  /* 设置服务器回调 */
  echo_node->set_recv_cb([](const std::shared_ptr<UdpNode> &udp_node,
                            std::vector<uint8_t> &data,
                            const net::InetAddr &source) {
    /* heavy线程处理数据 */
    auto task = [udp_node, data = std::move(data), source]() mutable {
      /* 处理数据 */
      data.push_back(data.size());

      /* 回到io线程发送数据 */
      udp_node->post_send_task(std::move(data), source);
    };

    udp_node->post_heavy_task(std::move(task));
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
      /* 创建唯一的 Abstract 套接字路径 */
      const std::string client_socket_path{abstract_socket_path_base +
                                           "_client_" +
                                           std::to_string(client_index)};

      /* 创建客户端 */
      const auto client =
          UdpNode::make_by_path(*loop_, UdpType::kAbstract, client_socket_path);
      ASSERT_TRUE(client);

      /* 使用promise确保回调完成 */
      const auto done = std::make_shared<std::promise<void>>();
      const auto future = done->get_future();

      /* 设置客户端回调 */
      client->set_recv_cb([&success_count, &expected_data,
                           &done](const std::shared_ptr<UdpNode> &udp_node,
                                  std::vector<uint8_t> &data,
                                  const net::InetAddr &source) {
        /* 校验数据 */
        EXPECT_EQ(data, expected_data);
        /* 统计正确收到的包 */
        success_count.fetch_add(1);
        /* 接收完毕 */
        done->set_value();
      });

      /* 启动客户端 */
      client->start();

      /* 客户端发送数据 (Unix 套接字路径) */
      const auto server_addr =
          net::InetAddr::make_abstract(abstract_socket_path_base + "_server");
      ASSERT_TRUE(server_addr.has_value());

      client->post_send_task(enco_data, server_addr.value());

      /* 等待数据发送 */
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

/*-------------------- UdpNode套接字stop测试 --------------------*/
TEST_F(KZCoreTest, UdpNodeStopTest) {
  using namespace net::udp;
  constexpr uint16_t server_port{8888};

  const std::vector<uint8_t> data{'H', 'i'};

  /* 创建并启动 Server */
  const auto server =
      UdpNode::make_by_any(*loop_, UdpType::kUdpIpv4, server_port);
  ASSERT_TRUE(server);

  std::atomic<int> recv_count{0};

  /* 回调保证 */
  auto first_packet_done = std::make_shared<std::promise<void>>();
  auto future = first_packet_done->get_future();

  server->set_recv_cb([&recv_count, &first_packet_done](
                          const std::shared_ptr<UdpNode> &udp_node,
                          std::vector<uint8_t> &data,
                          const net::InetAddr &source) {
    recv_count.fetch_add(1);

    /* 只有第一次收到包时，才通知测试主线程 */
    if (recv_count.load() == 1) {
      first_packet_done->set_value();
    }
  });

  server->start();

  /* 创建 Client */
  const auto client =
      UdpNode::make_by_any(*loop_, UdpType::kUdpIpv4, 0); // 随机端口
  ASSERT_TRUE(client);

  client->start();

  const auto server_addr =
      net::InetAddr::make_ipv4("127.0.0.1", server_port).value();

  /* 第一阶段：正常发送一个包 */
  client->post_send_task(data, server_addr);

  /* 等待 Server 收到第一个包的回应 */
  ASSERT_EQ(future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_EQ(recv_count.load(), 1);

  /* 第二阶段：调用 Stop */
  server->stop();

  /* 再次发送 */
  client->post_send_task(data, server_addr);
  client->post_send_task(data, server_addr);

  /* 等待并验证 */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /* stop生效，只收到了stop之前的包 */
  EXPECT_EQ(recv_count.load(), 1);
}