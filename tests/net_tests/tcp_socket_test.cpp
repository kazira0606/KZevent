#include <array>
#include <cstdint>
#include <future>
#include <memory>
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
      if (cfd < 0)
        break;

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
