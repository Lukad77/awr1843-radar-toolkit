#pragma once
// Fft.h — 自包含的迭代 radix-2 FFT（零外部依赖）。
//
// 选型理由：本流水线中所有变换点数构造上就是 2 的幂
// （RadarConfig::derive() 对 numRangeBins 上取整；多普勒点数 ==
// numChirpsPerFrame；numAngleBins 经过 pow2 校验），且点数很小
// （64..256），查表式 radix-2 微秒级完成。FftPlan 是接缝，
// 日后可换成 pffft/FFTW 而不动任何 stage。
//
// FftPlan 在构造期一次性预计算位反转置换与旋转因子表；
// forward() 零分配，且可多线程并发调用（构造后全部状态只读）。

#include <complex>
#include <cstddef>
#include <vector>

namespace radar {

class FftPlan {
public:
  // n 必须是 2 的幂（>= 1）；否则抛 std::invalid_argument。
  explicit FftPlan(std::size_t n);

  std::size_t size() const noexcept { return n_; }

  // 原位、非归一化正向 DFT：X[k] = sum x[n] e^{-2*pi*i*k*n/N}。
  void forward(std::complex<float> *x) const;

private:
  std::size_t n_;
  std::vector<std::size_t> rev_;        // 位反转置换表
  std::vector<std::complex<float>> tw_; // 旋转因子 W_n^k，k ∈ [0, n/2)
};

// 交换两半，使零频落在中心（n 必须为偶数）。
void fftshift(std::complex<float> *x, std::size_t n);

// 长度为 n 的对称 Hann 窗（n>=1；n==1 时返回 {1}）。
std::vector<float> makeHannWindow(std::size_t n);

} // namespace radar
