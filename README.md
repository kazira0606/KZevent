# KZevent

`KZevent` 是Linux下基于 C++17 的轻量异步事件驱动库。目前支持了TCP/UDP/UNIX Socket，具备较为完善的GTest覆盖以及Example和压力测试，自带轻量同步日志。

---

## 架构设计

`KZevent` 的核心是 `Loop` 组件，启动`Loop`会自动开启io线程和heavy线程：

| 运行线程 | 核心职责 |
| :--- | :--- |
| `io_thread` | 负责 `epoll_wait` 监听、系统调用 `read/write`、以及`on_split`切包逻辑。 |
| `heavy_thread` | 负责执行回调函数（如 stream:`on_fragment`、dgram:`on_read` ），确保回调逻辑不阻塞 IO。 |

* **水平扩展**：`Loop` 被设计为最小功能单元。如果需要更高的并发能力，可以通过创建多个 `Loop` 实例来分担负载。
* **协议无关**：自由定义粘包处理逻辑（如长度头、特殊分隔符等）。

---

## 核心特性

| 分类 | 特性 |
| :--- | :--- |
| **驱动模式** | 强制使用 ET 触发 |
| **网络协议** | TCP (Server/Client)， UDP (Server/Client)， Unix Stream (Path/Abstract) |
| **定时任务** | 支持单次、重复异步定时器 |

---

## 快速上手

### 1. TCP 服务端示例

```cpp
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

auto loop = std::make_unique<Loop>();
auto addr = InetAddr::make_ipv4("127.0.0.1", 7777).value();
auto server = TcpServer::make_tcp_server(*loop, addr);

/* 1. 定义切包逻辑 (运行在 IO 线程) */
server->set_split_cb([](const TcpServer::Session&, 
                        std::string_view slice) -> ssize_t {
    if (slice.size() < 4) return 0;
    uint32_t len{};
    std::copy(slice.data(), slice.data() + 4, reinterpret_cast<uint8_t*>(&len));
    /* 返回该包长度（未收满一包返回 0） */ 
    return (len + 4 <= slice.size()) ? (len + 4) : 0;
});

/* 2. 处理帧 (运行在 Heavy 线程) */
server->set_fragment_cb([](const TcpServer::Session&, 
                           std::vector<uint8_t>* fragment) {
    std::cout << "Received a complete frame!" << std::endl;
});

loop->start();
```

---

### 2. UDP 服务端示例

```cpp
auto loop = std::make_unique<Loop>();
auto addr = InetAddr::make_ipv4("127.0.0.1", 7777).value();
auto server = UdpServer::make_udp_server(*loop, addr);

server->set_read_cb([](const UdpServer::Session &session,
                  std::vector<uint8_t> data) {
    /* 回声 */
    session->post_send_task(std::move(data));
});

loop->start();
```

---

### 3. 异步定时器示例

```cpp
auto timer = Timer::make_timer(*loop);
auto holder = std::make_shared<int>(0);

/* 每隔1s执行一次 */
timer->post_repeat_task([holder]() {
    std::cout << "Timer tick: " << ++(*holder) << std::endl;
}, holder, 1000);
```

---

## 构建

| 项目 | 要求 |
| :--- | :--- |
| **操作系统** | Linux |
| **编译器** | 支持 C++17 的 GCC 或 Clang |
| **构建工具** | CMake 3.14+ |

---

## 压力测试

### 使用 wrk 1000 连接 30s 测试

| Thread Stats | Avg | Stdev | Max | +/- Stdev |
| :--- | :--- | :--- | :--- | :--- |
| Latency | 14.67ms | 302.44us | 21.05ms | 87.75% |
| Req/Sec | 34.05k | 695.05 | 35.35k | 63.67% |

| Latency Distribution |
| :---: |
| 50% : 14.67ms |
| 75% : 14.79ms |
| 90% : 14.91ms |
| 99% : 15.15ms |

| QPS |
| :--- |
| Requests/sec: 67645.00 |
| Transfer/sec: 4.39MB |

---

## 开发初衷

**学习导向**：本项目主要用于 Modern C++ 及 Linux 编程的学习。

---
