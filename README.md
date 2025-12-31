# KZevent

`KZevent` 是一个基于 **C++17** 开发的轻量级、高性能异步事件驱动网络库。本项目是我在学习 C++ 和 Linux 网络编程过程中的实践产物，旨在设计一个逻辑清晰、易于扩展的底层事件调度框架。

---

## 🏗️ 架构设计理念 (Dual-Thread Loop)

`KZevent` 的核心是 `Loop` 组件。与传统的单线程 Reactor 不同，本库采用了**读写与逻辑分离**的双线程架构：

| 组件名称 | 运行线程 | 核心职责 |
| :--- | :--- | :--- |
| **IO 线程** | `io_thread` | 负责 `epoll_wait` 监听、系统调用 `read/write`、以及流式协议的**切包 (Split)** 逻辑。 |
| **Heavy 线程** | `heavy_thread` | 负责执行所有用户定义的回调函数（如 `on_fragment`、`on_read` 等），确保业务逻辑不阻塞 IO。 |

### 设计优势
*   **非阻塞响应**：即便用户在回调函数中处理较重的业务逻辑，也不会阻塞 IO 线程对网络事件的实时响应。
*   **水平扩展**：`Loop` 被设计为最小功能单元。如果需要更高的并发能力，可以通过创建多个 `Loop` 实例来分担负载。
*   **协议无关**：本库不感知任何应用层协议。通过 `split_cb`，用户可以自由定义粘包处理逻辑（如长度头、特殊分隔符等）。

---

## 🚀 核心特性

| 分类 | 支持功能 |
| :--- | :--- |
| **网络协议** | TCP (Server/Client), UDP (Server/Client), Unix Domain Socket (Path/Abstract) |
| **驱动模式** | 强制使用 **Edge Triggered (ET)** 边缘触发，追求高效状态控制 |
| **定时任务** | 支持单次、重复异步定时器，支持任务持有者（LifeChecker）绑定以防悬空指针 |
| **任务调度** | 提供 `post_io_task` 和 `post_heavy_task` 接口，支持跨线程投递任务 |

---

## 📖 快速上手

### 1. TCP 服务端示例（带协议切包）
```cpp
auto loop = std::make_unique<Loop>();
auto addr = InetAddr::make_ipv4("127.0.0.1", 7777).value();
auto server = TcpServer::make_tcp_server(*loop, addr);

// 1. 定义切包逻辑 (运行在 IO 线程)
server->set_split_cb([](const auto&, std::string_view slice) -> ssize_t {
    if (slice.size() < 4) return 0;
    uint32_t len{};
    std::copy(slice.data(), slice.data() + 4, reinterpret_cast<uint8_t*>(&len));
    return (len + 4 <= slice.size()) ? (len + 4) : 0;
});

// 2. 处理业务帧 (运行在 Heavy 线程)
server->set_fragment_cb([](const auto& session, std::vector<uint8_t>* fragment) {
    std::cout << "Received a complete frame!" << std::endl;
});

loop->start();
```

---

### 2. 异步定时器
```cpp
auto timer = Timer::make_timer(*loop);
auto holder = std::make_shared<int>(0);

// 每隔 1s 执行一次，直到 holder 析构
timer->post_repeat_task([holder]() {
    std::cout << "Timer tick: " << ++(*holder) << std::endl;
}, holder, 1000);
```

---

## 🛠️ 构建与要求

| 项目 | 要求 |
| :--- | :--- |
| **操作系统** | Linux (依赖 `epoll` 与 `eventfd`) |
| **编译器** | 支持 **C++17** 的 GCC (建议 9+) 或 Clang |
| **构建工具** | CMake 3.10+ |

### 编译步骤
```bash
mkdir build
cd build
cmake ..
make
```

---

## 📝 开发初衷与免责声明

1.  **学习导向**：本项目主要用于练习 C++ 高级特性（如移动语义、模板编程、原子变量）及多线程架构设计。
2.  **性能预期**：本项目未进行大规模高并发压力测试，目标是提供一种优雅的事件库设计思想，而非追求极限吞吐量。
3.  **生产建议**：考虑到这是一个学习性质的项目，不建议直接用于严苛的生产环境，但非常欢迎用于学习交流。

---

## 🤝 交流与反馈

如果你对 `KZevent` 的双线程模型有任何建议，或者在使用过程中发现了 Bug，欢迎提交 **Issue** 或 **Pull Request**。
