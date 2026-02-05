#include <cstdint>
#include <iterator>

#include <sys/types.h>

#include "kzevent/stream_buffer.hpp"

namespace kzevent::core {
uint8_t *StreamBuffer::data() noexcept { return buffer_.data() + reader_; }

uint8_t *StreamBuffer::begin() noexcept { return buffer_.data() + reader_; }

uint8_t *StreamBuffer::end() noexcept { return buffer_.data() + writer_; }

ssize_t StreamBuffer::size() const noexcept { return writer_ - reader_; };

ssize_t StreamBuffer::space() const noexcept {
  return std::distance(buffer_.begin() + writer_, buffer_.end());
}

void StreamBuffer::push(const ssize_t len) noexcept { writer_ += len; }

void StreamBuffer::pop(const ssize_t len) noexcept {
  reader_ += len;

  if (reader_ >= writer_) {
    /* 一次取走所有数据 */
    shrink();
    reader_ = 0;
    writer_ = 0;
    return;
  }

  if (reader_ > static_cast<ssize_t>(buffer_.size()) / 2) {
    /* buffer 头部空余数据大于总容量一半 */
    compact();
  }
}

bool StreamBuffer::empty() const noexcept { return reader_ == writer_; }

void StreamBuffer::clear() noexcept {
  reader_ = 0;
  writer_ = 0;
}

void StreamBuffer::compact() {
  const ssize_t data_len = writer_ - reader_;

  std::copy(buffer_.begin() + reader_, buffer_.begin() + writer_,
            buffer_.begin());

  /* 重置指针 */
  reader_ = 0;
  writer_ = data_len;
}

void StreamBuffer::shrink() {
  if (buffer_.size() <= initial_size_) {
    /* 无需缩容 */
    shrink_count_ = 0;
    return;
  }

  if (shrink_count_ < kShrinkThreshold) {
    /* 未达到缩容次数 */
    ++shrink_count_;
    return;
  }

  shrink_count_ = 0;
  const size_t new_size = (buffer_.size() / kShrinkDivisor) < initial_size_
                              ? initial_size_
                              : buffer_.size() / kShrinkDivisor;
  buffer_.resize(new_size);
  buffer_.shrink_to_fit();
}
} // namespace kzevent::core