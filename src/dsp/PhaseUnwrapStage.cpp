#include "dsp/PhaseUnwrapStage.h"

#include <algorithm>
#include <cmath>

namespace radar {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

PhaseUnwrapStage::PhaseUnwrapStage(const RadarConfig &cfg,
                                   PhaseUnwrapParams params)
    : cfg_(cfg), p_(params) {
  if (p_.targetRangeBin >= 0)
    bin_ = p_.targetRangeBin;
  if (p_.dcWindowFrames < 2)
    p_.dcWindowFrames = 2;
  dcBuf_.resize(static_cast<std::size_t>(p_.dcWindowFrames));
}

int PhaseUnwrapStage::selectBin(const FrameBuffer &cube, std::size_t rx) const {
  const FrameShape s = cube.shape(); // {chirps, rx, rangeBins}

  // 以 bin 为单位的距离门；排除 DC bin。配置无剖面信息时
  // （rangeIdxToMeters == 0，如单测）回退到全频带。
  int lo = 1, hi = static_cast<int>(s.samples) - 1;
  if (cfg_.rangeIdxToMeters > 0.f) {
    lo = std::max(1, static_cast<int>(p_.minRangeM / cfg_.rangeIdxToMeters));
    hi = std::min(hi, static_cast<int>(p_.maxRangeM / cfg_.rangeIdxToMeters));
    if (lo > hi) {
      lo = 1;
      hi = static_cast<int>(s.samples) - 1;
    }
  }

  int best = lo;
  double bestMag = -1.0;
  for (int b = lo; b <= hi; ++b) {
    const double m = meanAmp(cube, rx, b);
    if (m > bestMag) {
      bestMag = m;
      best = b;
    }
  }
  return best;
}

double PhaseUnwrapStage::meanAmp(const FrameBuffer &cube, std::size_t rx,
                                 int b) const {
  const FrameShape s = cube.shape();
  if (b < 0 || static_cast<std::size_t>(b) >= s.samples || s.chirps == 0)
    return 0.0;
  double m = 0.0;
  for (std::size_t c = 0; c < s.chirps; ++c)
    m += std::abs(cube.at(c, rx, static_cast<std::size_t>(b)));
  return m / static_cast<double>(s.chirps);
}

void PhaseUnwrapStage::fitDcCenter() {
  // 中心化坐标下的 Kasa 最小二乘圆拟合：
  //   [Suu Suv][uc]   [ (Suuu + Suvv)/2 ]
  //   [Suv Svv][vc] = [ (Svuu + Svvv)/2 ]，圆心 = 点均值 + (uc, vc)。
  // 退化（点重合/共线/弧张角过小）：沿用旧圆心。
  const std::size_t n = dcCount_;
  if (n < 3)
    return;

  double mx = 0.0, my = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    mx += dcBuf_[i].real();
    my += dcBuf_[i].imag();
  }
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);

  double Suu = 0, Suv = 0, Svv = 0, Suuu = 0, Svvv = 0, Suvv = 0, Svuu = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const double u = dcBuf_[i].real() - mx;
    const double v = dcBuf_[i].imag() - my;
    Suu += u * u;
    Suv += u * v;
    Svv += v * v;
    Suuu += u * u * u;
    Svvv += v * v * v;
    Suvv += u * v * v;
    Svuu += v * u * u;
  }

  const double det = Suu * Svv - Suv * Suv;
  const double spread = Suu + Svv;
  // 相对条件数门限：细长/退化的弧会使 det << spread^2。
  if (spread <= 0.0 || det <= 1e-3 * spread * spread)
    return; // 沿用旧圆心

  const double b1 = 0.5 * (Suuu + Suvv);
  const double b2 = 0.5 * (Svvv + Svuu);
  const double uc = (b1 * Svv - b2 * Suv) / det;
  const double vc = (b2 * Suu - b1 * Suv) / det;

  dcCenter_ = {mx + uc, my + vc};
  dcValid_ = true;
}

bool PhaseUnwrapStage::process(FrameContext &ctx) {
  if (!ctx.valid || !ctx.rangeCube)
    return true;

  const FrameBuffer &cube = *ctx.rangeCube;
  const FrameShape s = cube.shape();
  if (s.chirps == 0 || s.rx == 0 || s.samples == 0)
    return true;

  const std::size_t rx = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(p_.rxIdx, 0)), s.rx - 1);

  // ---- bin 选择：首次锁定，之后是迟滞门控的峰值跟随 ----
  if (bin_ < 0) {
    bin_ = selectBin(cube, rx); // 首次锁定：无需桥接
  } else if (p_.targetRangeBin < 0 && p_.followPeak) {
    const int cand = selectBin(cube, rx);
    const bool qualifies =
        cand != bin_ &&
        meanAmp(cube, rx, cand) >
            static_cast<double>(p_.switchRatio) * meanAmp(cube, rx, bin_);
    if (qualifies) {
      if (cand == candBin_)
        ++candStreak_;
      else {
        candBin_ = cand;
        candStreak_ = 1;
      }
      if (candStreak_ >= p_.switchHoldFrames) {
        bin_ = cand;
        ++switches_;
        bridgePending_ = true; // 新观测点：首样本零增量
        candBin_ = -1;
        candStreak_ = 0;
        dcHead_ = dcCount_ = 0; // 新 bin 的静态相量不同，清窗重积
        dcValid_ = false;
      }
    } else {
      candBin_ = -1;
      candStreak_ = 0;
    }
  }
  if (bin_ < 0 || static_cast<std::size_t>(bin_) >= s.samples)
    return true;

  // ---- 质量指标：跟踪 bin 的原始 chirp 均幅度 ----
  double ampSum = 0.0;
  for (std::size_t c = 0; c < s.chirps; ++c)
    ampSum += std::abs(cube.at(c, rx, static_cast<std::size_t>(bin_)));
  const double trackAmp = ampSum / static_cast<double>(s.chirps);

  // ---- 帧观测相量 z：chirp 相干平均 + 邻域幅度加权合并 ----
  // 与 MATLAB 参考 extract_z_series_rx_v2 相同：先对帧内所有 chirp
  // 求复数均值（√chirps 相位噪声增益），再对 [bin-span, bin+span]
  // 按幅度加权相干合并——软化 bin 边界与切换处的不连续。
  const int span = std::max(p_.neighborSpan, 0);
  const int nbLo = std::max(0, bin_ - span);
  const int nbHi = std::min(static_cast<int>(s.samples) - 1, bin_ + span);
  std::complex<double> z{0.0, 0.0};
  double wSum = 0.0;
  for (int b = nbLo; b <= nbHi; ++b) {
    std::complex<double> zb{0.0, 0.0};
    for (std::size_t c = 0; c < s.chirps; ++c) {
      const FrameBuffer::Sample v = cube.at(c, rx, static_cast<std::size_t>(b));
      zb += std::complex<double>(v.real(), v.imag());
    }
    zb /= static_cast<double>(s.chirps);
    const double w = std::abs(zb);
    z += w * zb;
    wSum += w;
  }
  if (wSum > 0.0)
    z /= wSum;

  if (p_.dcCompensation) {
    dcBuf_[dcHead_] = z; // 原始值入窗，未补偿
    dcHead_ = (dcHead_ + 1) % dcBuf_.size();
    dcCount_ = std::min(dcCount_ + 1, dcBuf_.size());
    fitDcCenter();
  }
  const bool compensate = p_.dcCompensation && dcValid_ && dcCount_ >= 16;

  // ---- 帧级解缠：每帧一次 atan2，增量经 prevRaw_ 跨帧连续 ----
  // （旧实现逐 chirp 解缠再取帧均值：64 步/帧的解缠链上任何一个
  // 低 SNR chirp 注入的 ±2π 滑移都会永久污染累积器。）
  if (compensate)
    z -= dcCenter_;
  const double phi = std::atan2(z.imag(), z.real());

  if (!hasPrev_) {
    unwrapped_ = phi;
    hasPrev_ = true;
  } else if (bridgePending_) {
    // 换 bin 桥接：重新播种原始参考相位，增量记零 ——
    // 切换观测点绝不能伪造相位阶跃。
  } else {
    // 用 remainder() 把增量折回 (-pi, pi]：舍入到最近周期。
    unwrapped_ += std::remainder(phi - prevRaw_, 2.0 * kPi);
  }
  prevRaw_ = phi;
  bridgePending_ = false;

  const double mean = unwrapped_;
  if (!hasBase_) {
    baseMean_ = mean;
    hasBase_ = true;
  }

  ctx.unwrappedPhaseRad = static_cast<float>(mean);
  ctx.displacementMm = static_cast<float>((mean - baseMean_) *
                                          static_cast<double>(cfg_.lambdaM) /
                                          (4.0 * kPi) * 1000.0);
  ctx.phaseTrackBin = bin_;
  ctx.phaseTrackAmp = static_cast<float>(trackAmp);
  return true;
}

} // namespace radar
