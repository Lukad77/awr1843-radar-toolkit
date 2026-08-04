// test_core.cpp — Phase 0-2 地基的无依赖单元测试。
//
// 覆盖计划要求的正确性关键行为：
//   * 回绕安全的 uint32 序列号算术（SeqNum）
//   * 有界阻塞 SPSC 环：满 => 非阻塞 push 失败（不丢），
//     阻塞 push/pop 跨线程严格 FIFO 且无损
//   * FrameBuffer 连续内存索引
//   * BufferPool 回收复用（无逐帧分配）
//   * RadarConfig 派生 + 校验（单一事实源）
//
// 任何失败都返回非零，使 CTest 能报告通过/失败。

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "core/BufferPool.h"
#include "core/FrameBuffer.h"
#include "core/Interfaces.h"
#include "core/RadarConfig.h"
#include "core/SeqNum.h"
#include "core/SpscRing.h"

static int g_fail = 0;
static int g_total = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_total;                                                                 \
    if (!(cond)) {                                                             \
      ++g_fail;                                                                \
      std::cerr << "FAIL: " << #cond << " @ " << __FILE__ << ":" << __LINE__   \
                << "\n";                                                       \
    }                                                                          \
  } while (0)

using namespace radar;

static void test_seqnum() {
  CHECK(seq_lt(0xFFFFFFFFu, 0u)); // 回绕：最大值“紧贴在”0 之前
  CHECK(seq_gt(0u, 0xFFFFFFFFu));
  CHECK(seq_diff(5u, 2u) == 3);
  CHECK(seq_diff(1u, 0xFFFFFFFFu) == 2); // 跨边界的回绕距离
  CHECK(seq_ge(10u, 10u));
  CHECK(seq_le(10u, 10u));
  CHECK(seq_gap(100u, 103u) == 3); // 领先 3 个 => 缺口
  CHECK(seq_gap(100u, 99u) == -1); // 过期/重复包
}

static void test_spscring_basic() {
  SpscRing<int> q(4);
  CHECK(q.capacity() == 4);
  CHECK(q.try_push(1));
  CHECK(q.try_push(2));
  CHECK(q.try_push(3));
  CHECK(q.try_push(4));
  CHECK(!q.try_push(5)); // 满 => 非阻塞 push 失败（不丢、不阻塞）
  int v = 0;
  CHECK(q.try_pop(v) && v == 1);
  CHECK(q.try_pop(v) && v == 2);
  CHECK(q.size() == 2);
}

static void test_spscring_threaded() {
  SpscRing<std::uint64_t> q(64);
  const std::uint64_t N = 100000;
  std::uint64_t sum = 0;
  bool orderOk = true;

  std::thread cons([&] {
    std::uint64_t x = 0, prev = 0;
    bool first = true;
    while (q.pop(x)) {
      if (!first && x != prev + 1)
        orderOk = false; // 严格 FIFO 顺序
      prev = x;
      first = false;
      sum += x;
    }
  });

  for (std::uint64_t i = 1; i <= N; ++i)
    q.push(i); // 阻塞 => 无损背压
  q.close();
  cons.join();

  CHECK(orderOk);
  CHECK(sum == N * (N + 1) / 2); // 不丢不重
}

static void test_framebuffer() {
  FrameShape s{2, 4, 256};
  CHECK(s.total() == 2u * 4u * 256u);
  FrameBuffer fb(s);
  CHECK(fb.size() == s.total());
  CHECK(fb.index(0, 0, 0) == 0u);
  CHECK(fb.index(1, 3, 255) == (1u * 4u + 3u) * 256u + 255u);
  fb.at(1, 2, 3) = {1.5f, -2.5f};
  CHECK(fb.at(1, 2, 3).real() == 1.5f);
  CHECK(fb.at(1, 2, 3).imag() == -2.5f);
}

static void test_bufferpool() {
  auto pool = BufferPool<FrameBuffer>::create(
      [] { return std::make_unique<FrameBuffer>(FrameShape{1, 1, 256}); },
      [](FrameBuffer &b) {
        if (b.size())
          b.data()[0] = {};
      },
      2);
  CHECK(pool->free_count() == 2);

  FrameBuffer *addr1 = nullptr;
  {
    auto b = pool->acquire();
    addr1 = b.get();
    CHECK(pool->free_count() == 1);
  } // 此处归还给池
  CHECK(pool->free_count() == 2);

  auto b2 = pool->acquire();
  CHECK(b2.get() == addr1); // 复用同一对象（无新分配）
}

static void test_radarconfig() {
  RadarConfig c;
  c.numRxAnt = 4;
  c.numTxAnt = 1;
  c.numAdcSamples = 256;
  c.chirpStartIdx = 0;
  c.chirpEndIdx = 0;
  c.numLoops = 64;
  c.startFreqGHz = 77.f;
  c.idleTimeUs = 100.f;
  c.rampEndTimeUs = 60.f;
  c.freqSlopeMHzPerUs = 60.f;
  c.digOutSampleRateKsps = 10000;
  c.derive();

  CHECK(c.numChirpsPerFrame == 64);
  CHECK(c.numDopplerBins == 64);
  CHECK(c.numRangeBins == 256); // 已是 2 的幂
  CHECK(c.bytesPerSample == 4);
  CHECK(c.bytesPerFrame == static_cast<long>(64) * 4 * 256 * 4); // 262144
  CHECK(c.rangeResolutionMeters > 0.f);
  CHECK(c.maxRange > 0.f);

  std::string err;
  CHECK(c.validate(err));

  // 非 2 的幂的 ADC 采样数为距离 FFT 点数上取整。
  RadarConfig d = c;
  d.numAdcSamples = 200;
  d.derive();
  CHECK(d.numRangeBins == 256);

  // 非法配置被拒绝。
  RadarConfig bad;
  bad.numAdcSamples = 255; // 奇数
  bad.derive();
  std::string e2;
  CHECK(!bad.validate(e2));
  CHECK(!e2.empty());
}

int main() {
  test_seqnum();
  test_spscring_basic();
  test_spscring_threaded();
  test_framebuffer();
  test_bufferpool();
  test_radarconfig();

  std::cout << (g_total - g_fail) << "/" << g_total << " checks passed\n";
  if (g_fail) {
    std::cerr << g_fail << " CHECK(s) FAILED\n";
    return 1;
  }
  std::cout << "ALL PASS\n";
  return 0;
}
