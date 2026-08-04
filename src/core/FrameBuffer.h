#pragma once
// FrameBuffer.h — 单帧解析数据的连续、缓存友好存储。
//
// 替代旧的 vector<vector<vector<complex<float>>>>（三层堆间接、
// 局部性极差、逐帧重分配）。布局为行优先 [chirp][rx][sample]：
// idx = (chirp * numRx + rx) * numSamples + sample。
// 单块连续分配对 FFT/CFAR/NN 以及池化复用都是最优选择。

#include <complex>
#include <cstddef>
#include <vector>

namespace radar {

// 单个雷达帧已解析采样的逻辑维度。
struct FrameShape {
  std::size_t chirps = 0;
  std::size_t rx = 0;
  std::size_t samples = 0;

  std::size_t total() const noexcept { return chirps * rx * samples; }
  bool operator==(const FrameShape &o) const noexcept {
    return chirps == o.chirps && rx == o.rx && samples == o.samples;
  }
  bool operator!=(const FrameShape &o) const noexcept { return !(*this == o); }
};

class FrameBuffer {
public:
  using Sample = std::complex<float>;

  FrameBuffer() = default;
  explicit FrameBuffer(const FrameShape &s) { resize(s); }

  // 仅当总元素数变化时 resize 才重新分配。
  void resize(const FrameShape &s) {
    shape_ = s;
    if (data_.size() != s.total())
      data_.resize(s.total());
  }

  const FrameShape &shape() const noexcept { return shape_; }
  std::size_t size() const noexcept { return data_.size(); }

  Sample *data() noexcept { return data_.data(); }
  const Sample *data() const noexcept { return data_.data(); }

  std::size_t index(std::size_t chirp, std::size_t rx,
                    std::size_t sample) const noexcept {
    return (chirp * shape_.rx + rx) * shape_.samples + sample;
  }
  Sample &at(std::size_t chirp, std::size_t rx, std::size_t sample) noexcept {
    return data_[index(chirp, rx, sample)];
  }
  const Sample &at(std::size_t chirp, std::size_t rx,
                   std::size_t sample) const noexcept {
    return data_[index(chirp, rx, sample)];
  }

private:
  FrameShape shape_{};
  std::vector<Sample> data_;
};

} // namespace radar
