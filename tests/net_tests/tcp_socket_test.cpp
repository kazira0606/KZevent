#include <algorithm>
#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>

#include <gtest/gtest.h>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/tcp_socket.hpp"

using namespace kzevent;

/* 定义一个简单协议：Header(4字节长度) + Body */
std::vector<uint8_t> data_to_packet(const std::string &data) {
  std::vector<uint8_t> packet;
  uint32_t len = data.size();
  std::copy(reinterpret_cast<uint8_t *>(&len),
            reinterpret_cast<uint8_t *>(&len) + sizeof(len),
            std::back_inserter(packet));

  packet.insert(packet.end(), data.begin(), data.end());
  return packet;
};

/* 测试类 */
class KZTcpTest : public testing::Test {
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

/*-------------------- TCP client ipv4测试 --------------------*/
TEST_F(KZTcpTest, EchoTcpClientTestIpv4) {

  /* 定义数据：发送 "Hello world!" */
  const std::string raw_msg = "Hello world!";

  /* 校验回复数据 */
  std::vector<std::string> expect_resps{
      "Echo1: 456" + raw_msg + "AXAXYYY" + raw_msg,
      "Echo2: 7898799" + raw_msg + "MIHHH" + raw_msg,
      "Echo3: 46544" + raw_msg + "ZX" + raw_msg};

  /* 服务器端口 */
  const uint16_t server_port = 6666;

  /* 服务器创建等待 */
  const auto server_ready = std::make_shared<std::promise<void>>();
  const auto server_ready_future = server_ready->get_future();

  /* 创建一个 TCP 服务器 */
  std::thread server_thread{[this, &server_ready]() {
    int32_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    int32_t opt{1};
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    ASSERT_EQ(bind(lfd, (sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);

    /* 已经启动服务器 */
    server_ready->set_value();

    int32_t cfd = accept(lfd, nullptr, nullptr);
    if (cfd < 0) {
      ADD_FAILURE() << "failed to accept connection";
      return;
    }

    /* 读取数据 */
    std::array<uint8_t, UINT16_MAX> buf{};
    ssize_t n = read(cfd, buf.data(), buf.size());

    if (n > 0) {
      /* 收到数据(去掉head) */
      std::string data(buf.data(), buf.data() + n);
      std::string resp1 = "Echo1: 456" + data + "AXAXYYY" + data;
      std::string resp2 = "Echo2: 7898799" + data + "MIHHH" + data;
      std::string resp3 = "Echo3: 46544" + data + "ZX" + data;

      /* 构建发送包 */
      std::vector<uint8_t> packet1 = data_to_packet(resp1);
      std::vector<uint8_t> packet2 = data_to_packet(resp2);
      std::vector<uint8_t> packet3 = data_to_packet(resp3);

      EXPECT_GT(send(cfd, packet1.data(), packet1.size(), 0), 0);
      EXPECT_GT(send(cfd, packet2.data(), packet2.size(), 0), 0);
      EXPECT_GT(send(cfd, packet3.data(), packet3.size(), 0), 0);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    close(cfd);
    close(lfd);
  }};

  /* 创建TCP客户端 */
  const auto client_addr = net::InetAddr::make_ipv4("0.0.0.0", 0);
  ASSERT_TRUE(client_addr.has_value());

  const auto target_addr = net::InetAddr::make_ipv4("127.0.0.1", server_port);
  ASSERT_TRUE(target_addr.has_value());

  const auto client =
      net::tcp::TcpClient::make_tcp_client(*loop_, client_addr.value());
  ASSERT_TRUE(client);

  /* 统计收到的fragmen个数 */
  std::atomic<int32_t> fragment_count{0};

  /* 收包等待 */
  const auto done = std::make_shared<std::promise<void>>();
  const auto done_future = done->get_future();

  /* 断连等待 */
  const auto disconnect_ready = std::make_shared<std::promise<void>>();
  const auto disconnect_future = disconnect_ready->get_future();

  /* 设置拆包逻辑 */
  client->set_split_cb([](const auto &, auto stream_slice) -> ssize_t {
    if (stream_slice.size() < 4) {
      /* 没有收到完整的包->继续等待 */
      return 0;
    }

    /* 校验长度 */
    uint32_t data_len{};
    std::copy(stream_slice.data(), stream_slice.data() + 4,
              reinterpret_cast<uint8_t *>(&data_len));
    if (data_len > (stream_slice.size() - 4)) {
      /* 没有收到完整的包->继续等待 */
      return 0;
    }

    /* 返回包长度长度 */
    return 4 + data_len;
  });

  /* 设置包处理逻辑 */
  client->set_fragment_cb([&](const auto &, auto fragment) {
    /* 校验数据长度 */
    uint32_t data_len{};
    std::copy(fragment.data(), fragment.data() + 4,
              reinterpret_cast<uint8_t *>(&data_len));
    EXPECT_EQ(data_len, fragment.size() - 4);

    /* 校验数据 */
    std::string data{fragment.data() + 4, fragment.data() + fragment.size()};
    EXPECT_EQ(data, expect_resps[fragment_count]);

    ++fragment_count;

    if (fragment_count == 3) {
      done->set_value();
    }
  });

  /* 设置异常断开处理逻辑 */
  client->set_error_cb([&](const auto &) {
    ADD_FAILURE() << "disconnected or connection error";
    done->set_value();
  });

  /* 设置正常断连逻辑 */
  bool normal_disconnect{false};
  client->set_disconnect_cb([&](const auto &) {
    normal_disconnect = true;
    disconnect_ready->set_value();
  });

  /* 等待server启动 */
  if (server_ready_future.wait_for(std::chrono::seconds(3)) !=
      std::future_status::ready) {
    ADD_FAILURE() << "timed out waiting for server ready";
  }

  /* 连接并发送数据 */
  client->connect(target_addr.value(), 1000);
  client->post_send_task(raw_msg);

  /* 等待验证 */
  if (done_future.wait_for(std::chrono::seconds(3)) !=
      std::future_status::ready) {
    ADD_FAILURE() << "timed out waiting for packets";
  }

  EXPECT_EQ(fragment_count.load(), 3);

  if (disconnect_future.wait_for(std::chrono::seconds(3)) !=
      std::future_status::ready) {
    ADD_FAILURE() << "timed out waiting for disconnect";
  }
  EXPECT_TRUE(normal_disconnect);
  server_thread.join();
}

/*-------------------- TCP client 重启测试 --------------------*/
TEST_F(KZTcpTest, ClientStopAndRestartTest) {
  const uint16_t server_port = 8888;
  const std::string msg = "Restart test";

  const auto server_ready = std::make_shared<std::promise<void>>();
  auto server_ready_future = server_ready->get_future();

  /* 循环两次accept应对两次连接 */
  std::thread server_thread([&, server_ready]() {
    int32_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    int32_t opt{1};
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    ASSERT_EQ(bind(lfd, (sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);

    server_ready->set_value();

    for (int32_t i{0}; i < 2; ++i) {
      int32_t cfd = accept(lfd, nullptr, nullptr);
      if (cfd < 0) {
        ADD_FAILURE() << "failed to accept connection";
        return;
      }

      char buf[1024];
      ssize_t n = read(cfd, buf, sizeof(buf));
      if (n > 0) {
        /* 回声数据包 */
        auto packet = data_to_packet("Echo");
        send(cfd, packet.data(), packet.size(), 0);
      }
      close(cfd);
    }
    close(lfd);
  });

  server_ready_future.wait();

  for (int32_t i{0}; i < 2; ++i) {
    const auto done = std::make_shared<std::promise<void>>();
    const auto done_future = done->get_future();

    auto client = net::tcp::TcpClient::make_tcp_client(
        *loop_, net::InetAddr::make_ipv4("0.0.0.0", 0).value());

    client->set_split_cb([](const auto &, auto stream_slice) -> ssize_t {
      if (stream_slice.size() < 4) {
        /* 没有收到完整的包->继续等待 */
        return 0;
      }

      /* 校验长度 */
      uint32_t data_len{};
      std::copy(stream_slice.data(), stream_slice.data() + 4,
                reinterpret_cast<uint8_t *>(&data_len));
      if (data_len > (stream_slice.size() - 4)) {
        /* 没有收到完整的包->继续等待 */
        return 0;
      }

      /* 返回包长度长度 */
      return 4 + data_len;
    });

    client->set_fragment_cb([&](const auto &, auto) { done->set_value(); });

    client->set_error_cb([&](const auto &) { done->set_value(); });

    client->connect(net::InetAddr::make_ipv4("127.0.0.1", server_port).value(),
                    1000);
    client->post_send_task(msg);

    EXPECT_EQ(done_future.wait_for(std::chrono::seconds(3)),
              std::future_status::ready)
        << "failed at iteration " << i;

    /* 重启 */
    client->stop();
  }

  if (server_thread.joinable()) {
    server_thread.join();
  }
}

/*-------------------- TCP client 大数据测试 --------------------*/
TEST_F(KZTcpTest, TcpBigDataTest) {
  /* 准备 100MB 的随机测试数据 */
  const size_t kBigDataSize = 100 * 1024 * 1024;
  std::vector<uint8_t> big_data_raw(kBigDataSize);

  /* 随机数填充大数据 */
  std::mt19937 rng(std::random_device{}());
  std::generate(big_data_raw.begin(), big_data_raw.end(),
                [&]() { return static_cast<uint8_t>(rng()); });

  const uint16_t server_port = 9999;
  const auto server_ready = std::make_shared<std::promise<void>>();
  const auto server_ready_future = server_ready->get_future();

  const auto transfer_done = std::make_shared<std::promise<void>>();
  const auto transfer_done_future = transfer_done->get_future();

  /* server接收 100MB 数据并校验 */
  std::thread server_thread([&]() {
    int32_t lfd = socket(AF_INET, SOCK_STREAM, 0);
    int32_t opt{1};
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    ASSERT_EQ(bind(lfd, (sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);

    server_ready->set_value();

    int32_t cfd = accept(lfd, nullptr, nullptr);
    if (cfd < 0) {
      ADD_FAILURE() << "failed to accept connection";
      return;
    }

    /* 获取帧头 */
    uint32_t expected_len{0};
    ssize_t head_n = read(cfd, &expected_len, 4);
    ASSERT_EQ(head_n, 4);
    ASSERT_EQ(expected_len, kBigDataSize);

    /* 收满 100MB */
    std::vector<uint8_t> received_data;
    received_data.reserve(kBigDataSize);
    std::array<uint8_t, 64 * 1024> chunk{};

    size_t total_read = 0;
    while (total_read < kBigDataSize) {
      ssize_t n = read(cfd, chunk.data(), chunk.size());
      if (n <= 0)
        break;
      received_data.insert(received_data.end(), chunk.begin(),
                           chunk.begin() + n);
      total_read += n;
    }

    /* 校验数据 */
    bool is_equal = std::equal(big_data_raw.begin(), big_data_raw.end(),
                               received_data.begin());
    if (is_equal) {
      transfer_done->set_value();
    } else {
      ADD_FAILURE() << "Data corruption detected in big data transfer!";
    }

    close(cfd);
    close(lfd);
  });

  server_ready_future.wait();

  /* 创建TCP客户端 */
  const auto client_addr = net::InetAddr::make_ipv4("0.0.0.0", 0);
  ASSERT_TRUE(client_addr.has_value());

  const auto target_addr = net::InetAddr::make_ipv4("127.0.0.1", server_port);
  ASSERT_TRUE(target_addr.has_value());

  const auto client =
      net::tcp::TcpClient::make_tcp_client(*loop_, client_addr.value());
  ASSERT_TRUE(client);

  client->connect(target_addr.value(), 1000);

  /* 发送数据转str（构造帧函数只写了string的版本） */
  std::string big_data_str(big_data_raw.begin(), big_data_raw.end());

  client->post_send_task(data_to_packet(big_data_str));

  auto status = transfer_done_future.wait_for(std::chrono::seconds(3));
  EXPECT_EQ(status, std::future_status::ready)
      << "Big data transfer timed out or failed";

  server_thread.join();
}

/*-------------------- TCP server + client 并发测试 --------------------*/
TEST_F(KZTcpTest, TcpServerTest) {
  struct ServerSessionCtx {
    std::vector<std::string> received_msgs;
  };

  const int32_t MSGS_PER_CLIENT{5};
  const uint16_t port = 7777;

  auto server_addr = net::InetAddr::make_ipv4("127.0.0.1", port);
  auto server =
      net::tcp::TcpServer::make_tcp_server(*loop_, server_addr.value());
  ASSERT_TRUE(server);

  /* 切包 */
  server->set_split_cb([](const auto &, auto slice) -> ssize_t {
    if (slice.size() < 4)
      return 0;
    uint32_t len{};
    std::copy(slice.data(), slice.data() + 4,
              reinterpret_cast<uint8_t *>(&len));
    return (len + 4 <= slice.size()) ? (len + 4) : 0;
  });

  /* 业务逻辑 */
  server->set_fragment_cb([&](const auto &session, auto fragment) {
    if (!session->get_user_context()) {
      session->set_user_context(std::make_shared<ServerSessionCtx>());
    }
    auto ctx =
        std::static_pointer_cast<ServerSessionCtx>(session->get_user_context());

    /* 消息存储在每个客户端的contex中 */
    std::string msg(fragment.begin() + 4, fragment.end());
    ctx->received_msgs.push_back(std::move(msg));

    /* 收 MSGS_PER_CLIENT 个消息后逆序发回 */
    if (ctx->received_msgs.size() == MSGS_PER_CLIENT) {
      for (int32_t i{MSGS_PER_CLIENT - 1}; i >= 0; --i) {
        session->post_send_task(data_to_packet(ctx->received_msgs[i]));
      }
    }
  });

  std::atomic<int32_t> total_finished_clients{0};
  const auto all_done_promise = std::make_shared<std::promise<void>>();
  auto all_done_future = all_done_promise->get_future();

  /* 客户端 */
  struct ClientInstance {
    std::shared_ptr<net::tcp::TcpClient> client;
    std::vector<std::string> sent_msgs;
    std::vector<std::string> received_msgs;
  };

  std::vector<std::shared_ptr<ClientInstance>> clients;
  auto client_bind_addr = net::InetAddr::make_ipv4("0.0.0.0", 0).value();

  /* 创建 KZEVENT_CLIENT_NUM 个客户端并创建业务数据 */
  for (int32_t client_index{0}; client_index < KZEVENT_CLIENT_NUM;
       ++client_index) {
    auto inst = std::make_shared<ClientInstance>();

    for (int32_t str_index{0}; str_index < MSGS_PER_CLIENT; ++str_index) {
      inst->sent_msgs.push_back("Client" + std::to_string(client_index) +
                                "_Msg" + std::to_string(str_index) + "_" +
                                std::to_string(client_index * str_index));
    }

    inst->client =
        net::tcp::TcpClient::make_tcp_client(*loop_, client_bind_addr);

    /* 客户端切包 */
    inst->client->set_split_cb([](const auto &, auto slice) -> ssize_t {
      if (slice.size() < 4)
        return 0;
      uint32_t len{};
      std::copy(slice.data(), slice.data() + 4,
                reinterpret_cast<uint8_t *>(&len));
      return (len + 4 <= slice.size()) ? (len + 4) : 0;
    });

    std::weak_ptr<ClientInstance> weak_inst = inst;

    /* 客户端业务逻辑 */
    inst->client->set_fragment_cb([weak_inst, &total_finished_clients,
                                   all_done_promise, MSGS_PER_CLIENT](
                                      const auto &, auto fragment) {
      auto locked_inst = weak_inst.lock();
      if (!locked_inst)
        return;

      std::string msg(fragment.begin() + 4, fragment.end());
      locked_inst->received_msgs.push_back(std::move(msg));

      /* 校验反向发回的数据 */
      if (locked_inst->received_msgs.size() == MSGS_PER_CLIENT) {
        for (int32_t recv_index{0}; recv_index < MSGS_PER_CLIENT;
             ++recv_index) {
          EXPECT_EQ(locked_inst->received_msgs[recv_index],
                    locked_inst->sent_msgs[MSGS_PER_CLIENT - 1 - recv_index]);
        }
        if (++total_finished_clients == KZEVENT_CLIENT_NUM) {
          all_done_promise->set_value();
        }
      }
    });

    clients.push_back(std::move(inst));
  }

  /* 启动客户端 */
  auto target_addr = net::InetAddr::make_ipv4("127.0.0.1", port).value();
  for (auto &inst : clients) {
    inst->client->connect(target_addr, 1000);
    for (const auto &s : inst->sent_msgs) {
      inst->client->post_send_task(data_to_packet(s));
    }
  }

  /* 等待所有客户端完成 */
  auto status = all_done_future.wait_for(std::chrono::seconds(10));
  EXPECT_EQ(status, std::future_status::ready)
      << "Timeout! Finished: " << total_finished_clients.load();

  /* 确认所有客户端完成 */
  EXPECT_EQ(total_finished_clients.load(), KZEVENT_CLIENT_NUM);
}
