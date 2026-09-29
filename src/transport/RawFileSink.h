#pragma once
// RawFileSink.h — IResultSink：把重组后的原始帧无损落盘。
//
// 产物与 radar_capture 保持一致，便于离线工具链直接复用：
//   <output>               原始 ADC 帧（frameBytes 定长，顺序 = 提交顺序）
//   <output>.frames.csv    file_frame,wire_frame（文件帧号 ↔ 线上帧号）
//   <output>.stats.txt     帧数/尺寸统计（可附加传输层统计，见 setExtraStats）
//
// 运行在 Pipeline worker 线程上：写盘会阻塞 worker —— 这正是期望的背压来源
// （上游由 FrameSpool 吸收，生产者永不阻塞），数据路径上**绝不丢帧**；
// 尺寸不符或 ctx.valid=false 的帧只计数不落盘（绝不静默写残帧）。

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>

#include "core/Interfaces.h"

namespace radar {

class RawFileSink : public IResultSink {
public:
  // 传输层统计（重组丢包等）由调用方提供，flush() 时追加写入 stats.txt，
  // 避免两个组件同时写同一个文件。
  using StatsProvider = std::function<std::string()>;

  RawFileSink(const std::string &outputPath, std::size_t frameBytes);
  ~RawFileSink() override;

  RawFileSink(const RawFileSink &) = delete;
  RawFileSink &operator=(const RawFileSink &) = delete;

  bool ok() const { return ok_; }
  const std::string &lastError() const { return err_; }
  void setExtraStats(StatsProvider provider) { extra_ = std::move(provider); }

  void consume(const FrameContext &ctx) override;
  void flush() override;

  std::uint64_t savedFrames() const { return saved_; }
  std::uint64_t skippedFrames() const { return skipped_; }
  std::uint64_t shortFrames() const { return short_; }

private:
  std::string output_;
  std::size_t frameBytes_;
  std::ofstream raw_, index_;
  StatsProvider extra_;
  bool ok_ = false;
  std::string err_;
  std::uint64_t saved_ = 0, skipped_ = 0, short_ = 0;
  bool flushed_ = false;
};

} // namespace radar
