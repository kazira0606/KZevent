#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <sys/types.h>
#include <type_traits>
#include <vector>

namespace kzevent::core {
class StreamBuffer {
public:
  explicit StreamBuffer(size_t initial_size)
      : buffer_(initial_size), initial_size_(initial_size) {}

  ~StreamBuffer() = default;

  // 禁止拷贝，允许移动
  StreamBuffer(const StreamBuffer &) = delete;

  StreamBuffer &operator=(const StreamBuffer &) = delete;

  StreamBuffer(StreamBuffer &&) = default;

  StreamBuffer &operator=(StreamBuffer &&) = default;

  template <typename Iterator>
  void insert(const Iterator &begin, const Iterator &end);

  uint8_t *data() noexcept;

  uint8_t *begin() noexcept;

  uint8_t *end() noexcept;

  [[nodiscard]] ssize_t size() const noexcept;

  [[nodiscard]] ssize_t space() const noexcept;

  void push(ssize_t len) noexcept;

  void pop(ssize_t len) noexcept;

  [[nodiscard]] bool empty() const noexcept;

  void clear() noexcept;

private:
  /* 移动数据至头部 */
  void compact();

  /* 缩容 */
  void shrink();

  std::vector<uint8_t> buffer_{};
  ssize_t reader_{0};
  ssize_t writer_{0};

  /* 缩容计数器 */
  uint8_t shrink_count_{0};
  size_t initial_size_{0};
  constexpr static uint8_t ShrinkThreshold = 10;
};

/*-------------------- 模板实现 --------------------*/
template <typename Iterator>
void StreamBuffer::insert(const Iterator &begin, const Iterator &end) {
  /* 只允许迭代器指向的元素为 uint8_t / char */
  static_assert(
      std::is_same_v<typename std::iterator_traits<Iterator>::value_type,
                     uint8_t> ||
          std::is_same_v<typename std::iterator_traits<Iterator>::value_type,
                         char>,
      "invalid data type");

  const auto data_len = std::distance(begin, end);

  if (data_len > std::distance(buffer_.begin() + writer_, buffer_.end())) {
    /* 尾部容量不足扩容 */
    buffer_.resize(writer_ + data_len);
  }

  std::copy(begin, end, buffer_.begin() + writer_);
  writer_ += data_len;
}
} // namespace kzevent::core
