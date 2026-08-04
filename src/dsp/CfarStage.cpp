#include "dsp/CfarStage.h"

#include <algorithm>
#include <cmath>

namespace radar {

CfarStage::CfarStage(const RadarConfig &cfg, CfarParams params)
    : cfg_(cfg), p_(params) {}

bool CfarStage::process(FrameContext &ctx) {
  if (!ctx.valid || !ctx.rdMap)
    return true;

  const int D = cfg_.numDopplerBins;
  const int B = cfg_.numRangeBins;
  if (D <= 0 || B <= 0 ||
      ctx.rdMap->size() !=
          static_cast<std::size_t>(D) * static_cast<std::size_t>(B)) {
    ctx.valid = false; // 配置与生产者之间的维度漂移
    return true;
  }

  auto dets = std::make_shared<std::vector<Detection>>();
  const float *map = ctx.rdMap->data();
  constexpr float kNoiseFloorEps = 1e-12f; // 防止静默行上除零

  for (int d = 0; d < D && static_cast<int>(dets->size()) < p_.maxDetections;
       ++d) {
    const float *row = map + static_cast<std::size_t>(d) * B;

    for (int cut = 0; cut < B; ++cut) {
      // 训练窗，在行边缘处单侧截断。
      float sum = 0.f;
      int cnt = 0;
      const int loA = std::max(0, cut - p_.guardCells - p_.trainingCells);
      const int loB = cut - p_.guardCells - 1;
      for (int i = loA; i <= loB; ++i) {
        sum += row[i];
        ++cnt;
      }
      const int hiA = cut + p_.guardCells + 1;
      const int hiB = std::min(B - 1, cut + p_.guardCells + p_.trainingCells);
      for (int i = hiA; i <= hiB; ++i) {
        sum += row[i];
        ++cnt;
      }
      if (cnt == 0)
        continue;

      const float noise =
          std::max(sum / static_cast<float>(cnt), kNoiseFloorEps);
      // 按实际训练单元数计算的 CA-CFAR 门限系数。
      const float alpha = static_cast<float>(
          cnt * (std::pow(static_cast<double>(p_.pfa), -1.0 / cnt) - 1.0));
      if (row[cut] <= alpha * noise)
        continue;

      // 沿距离维的局部峰门控：每个目标簇只出一条检测。
      if (cut > 0 && row[cut] < row[cut - 1])
        continue;
      if (cut < B - 1 && row[cut] <= row[cut + 1])
        continue;

      Detection det;
      det.rangeBin = cut;
      det.dopplerBin = d;
      det.rangeM = cut * cfg_.rangeIdxToMeters;
      det.velocityMps = (d - D / 2) * cfg_.dopplerResolutionMps;
      det.snrDb = 10.f * std::log10(row[cut] / noise);
      dets->push_back(det);
      if (static_cast<int>(dets->size()) >= p_.maxDetections)
        break;
    }
  }

  ctx.detections = std::move(dets);
  return true;
}

} // namespace radar
