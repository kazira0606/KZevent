#include <string>
#include <vector>

#include "kzevent/net/inet.hpp"
#include "kzevent/net/tcp_socket.hpp"

int main() {
  using namespace kzevent::core;
  using namespace kzevent::net;
  using namespace kzevent::net::tcp;

  /* 创建loop */
  auto loop = std::make_unique<Loop>();
  loop->start();

  auto server_addr = InetAddr::make_ipv4("127.0.0.1", 7777).value();
  auto server = TcpServer::make_tcp_server(*loop, server_addr);

  /* server */
  auto http_split_cb = [](const TcpServer::Session &,
                          std::string_view slice) -> ssize_t {
    /* 查找结束段 */
    auto pos = slice.find("\r\n\r\n");
    if (pos != std::string::npos) {
      return static_cast<ssize_t>(pos + 4);
    }
    return 0;
  };

  /* 响应http */
  auto http_fragment_cb = [](const TcpServer::Session &session,
                             const std::vector<uint8_t>&) {
    /* 直接回复http报文 */
    static const std::string http_resp = "HTTP/1.1 200 OK\r\n"
                                         "Content-Length: 13\r\n"
                                         "Connection: keep-alive\r\n"
                                         "\r\n"
                                         "Hello!";

    session->post_send_task(http_resp);
  };

  server->set_split_cb(http_split_cb);
  server->set_fragment_cb(http_fragment_cb);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
  }
}
