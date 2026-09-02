// app.js — AWR1843 实时显示前端逻辑（原生 JS，无构建工具链）。
//
// 数据流：WebSocket 二进制帧（协议 v1，见 src/web/WireProtocol.h）
//   -> DataView 校验 magic/version -> TypedArray 零拷贝切片 -> 写入
//   滚动缓冲 -> requestAnimationFrame 统一重绘（收包与渲染解耦，
//   浏览器每帧最多重绘一次）。
//
// 呼吸处理（仅显示用途，算法参数与 plot_breathing.py 对齐）：
//   - RBJ biquad 带通 0.1–0.5 Hz（系数按 meta.fps 现算）滤 displacementMm
//   - 呼吸率：30 s 滑窗内 0.05–0.7 Hz Goertzel 谱峰 × 60（bpm）
//   - 质量门控：trackAmp < 0.4×滑动中位数 => 该样本呼吸波形置 null（灰断）
//
// 控制栏：连接/断开（手动断开不自动重连）、暂停/继续（冻结画面，
// 数据继续入缓冲，便于查看细节）、清空（复位时间序列与滤波器状态）、
// WS 地址输入框（默认取 ?ws=/?port= 参数，回车即重连）。

"use strict";

// WS 地址可用 URL 参数覆盖：?ws=ws://host:port 或 ?port=8766（默认 8765）。
const _qs = new URLSearchParams(location.search);
const WS_URL = _qs.get("ws") ||
    `ws://localhost:${_qs.get("port") || 8765}`;
const MAGIC = 0x31574452; // 'RDW1'
const VERSION = 1;
const WINDOW_SEC = 60;    // 滚动窗口
const RATE_WIN_SEC = 30;  // 呼吸率估计窗口
const GATE_RATIO = 0.4;   // trackAmp 门控阈值（×滑动中位数）

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
let meta = null;        // 服务端 meta JSON
let cap = 1200;         // 滚动缓冲容量 = WINDOW_SEC * fps
let n = 0;              // 已积累样本数（<= cap）
let tBuf, phaseBuf, dispBuf, breathBuf, ampBuf; // 滚动缓冲（Float64Array）
let lastFrame = null;   // 最新一帧的波形/距离谱（TypedArray 拷贝）
let dirty = false;      // 有新数据待重绘
let biquad = null;      // 带通滤波器状态
let frameCount = 0;

// 控制状态（由控制栏按钮驱动）
let ws = null;          // 当前连接（null = 未连接）
let manualClose = false;// 手动断开：不自动重连
let paused = false;     // 暂停 = 冻结图表刷新（数据继续入缓冲）

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------------------
// RBJ biquad 带通（constant skirt gain），系数按帧率现算
// ---------------------------------------------------------------------------
function makeBandpass(fs, fLo, fHi) {
  const f0 = Math.sqrt(fLo * fHi);           // 几何中心
  const bw = (fHi - fLo) / f0;               // 相对带宽（倍频程近似）
  const w0 = 2 * Math.PI * f0 / fs;
  const q = f0 / (fHi - fLo);
  const alpha = Math.sin(w0) / (2 * q);
  const b0 = alpha, b1 = 0, b2 = -alpha;
  const a0 = 1 + alpha, a1 = -2 * Math.cos(w0), a2 = 1 - alpha;
  const c = { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0,
              x1: 0, x2: 0, y1: 0, y2: 0, primed: false };
  c.step = (x) => {
    if (!c.primed) { c.x1 = c.x2 = x; c.y1 = c.y2 = 0; c.primed = true; }
    const y = c.b0 * x + c.b1 * c.x1 + c.b2 * c.x2 - c.a1 * c.y1 - c.a2 * c.y2;
    c.x2 = c.x1; c.x1 = x; c.y2 = c.y1; c.y1 = y;
    return y;
  };
  return c;
}

// 呼吸率（bpm）：RATE_WIN_SEC 窗内 0.05–0.7 Hz 扫 Goertzel 谱峰。
function estimateBpm() {
  if (!meta) return null;
  const fs = meta.fps;
  const len = Math.min(n, Math.round(RATE_WIN_SEC * fs));
  if (len < RATE_WIN_SEC * fs) return null; // 窗口未满不出数
  const start = (writeIdx() - len + cap) % cap;
  // 去均值副本（Goertzel 对直流敏感）
  const seg = new Float64Array(len);
  let mean = 0;
  for (let i = 0; i < len; ++i) { seg[i] = breathBuf[(start + i) % cap] || 0; mean += seg[i]; }
  mean /= len;
  for (let i = 0; i < len; ++i) seg[i] -= mean;
  let bestF = 0, bestP = -1;
  for (let f = 0.05; f <= 0.7; f += 0.01) {
    const w = 2 * Math.PI * f / fs, coeff = 2 * Math.cos(w);
    let s1 = 0, s2 = 0;
    for (let i = 0; i < len; ++i) { const s0 = seg[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    const p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (p > bestP) { bestP = p; bestF = f; }
  }
  return bestF * 60;
}

function writeIdx() { return frameCount % cap; } // 下一个写入位置

// 滑动中位数（trackAmp 门控用；对最近 len 个样本取中位）
function rollingMedianAmp() {
  const len = Math.min(n, cap);
  if (len === 0) return 0;
  const a = [];
  for (let i = 0; i < len; ++i) a.push(ampBuf[i]);
  a.sort((x, y) => x - y);
  return a[Math.floor(a.length / 2)];
}

// ---------------------------------------------------------------------------
// 图表（uPlot）：先建骨架，meta 到达后按 nWave/nBins 重建 x 轴
// ---------------------------------------------------------------------------
const dark = { stroke: "#8b949e", grid: { stroke: "#21262d" }, ticks: { stroke: "#30363d" } };
function axes(xLabel, yLabel) {
  return [ { ...dark, label: xLabel }, { ...dark, label: yLabel } ];
}
function mkChart(el, title, series, xLabel, yLabel, w) {
  const opts = {
    width: w, height: 220,
    scales: { x: { time: false } },
    axes: axes(xLabel, yLabel),
    series: [{}, ...series],
    legend: { live: false },
    cursor: { drag: { x: false, y: false } },
  };
  return new uPlot(opts, [[0], ...series.map(() => [0])], el);
}

let chAdc = null, chRange = null, chPhase = null, chBreath = null;
function cardWidth() {
  return Math.max(360, $("chart-adc").parentElement.clientWidth - 8);
}
function initCharts() {
  const w = cardWidth();
  chAdc = mkChart($("chart-adc"), "adc",
    [ { label: "I", stroke: "#58a6ff", width: 1 },
      { label: "Q", stroke: "#f778ba", width: 1 } ], "采样点", "ADC");
  chRange = mkChart($("chart-range"), "range",
    [ { label: "幅度 (dB)", stroke: "#3fb950", width: 1.2 },
      { label: "跟踪 bin", stroke: "#d29922", width: 1, points: { show: true, size: 6 }, paths: () => null } ],
    "距离 (m)", "dB");
  chPhase = mkChart($("chart-phase"), "phase",
    [ { label: "相位 (rad)", stroke: "#58a6ff", width: 1.2 },
      { label: "位移 (mm)", stroke: "#3fb950", width: 1.2, scale: "mm" } ],
    "时间 (s)", "rad"); // 位移挂独立 scale "mm"（uPlot 自动建 scale，仅曲线无轴）
  chBreath = mkChart($("chart-breath"), "breath",
    [ { label: "呼吸位移 (mm)", stroke: "#f85149", width: 1.4 } ], "时间 (s)", "mm");
  window.addEventListener("resize", () => {
    const w2 = cardWidth();
    for (const c of [chAdc, chRange, chPhase, chBreath]) c && c.setSize({ width: w2, height: 220 });
  });
}

// ---------------------------------------------------------------------------
// WebSocket + 协议解码
// ---------------------------------------------------------------------------
function onMeta(m) {
  meta = m;
  cap = Math.round(WINDOW_SEC * meta.fps);
  n = 0; frameCount = 0;
  tBuf = new Float64Array(cap);
  phaseBuf = new Float64Array(cap);
  dispBuf = new Float64Array(cap);
  breathBuf = new Float64Array(cap);
  ampBuf = new Float64Array(cap);
  biquad = makeBandpass(meta.fps, 0.1, 0.5);
  $("meta").textContent =
    `源 ${meta.source} | ${meta.nWave} 采样 × ${meta.nBins} bins | ` +
    `${meta.fps} fps | Δr=${(meta.rangeIdxToMeters).toFixed(4)} m/bin`;
}

function onFrame(buf) {
  if (!meta) return; // meta 未到先丢
  const dv = new DataView(buf);
  if (buf.byteLength < 36 || dv.getUint32(0, true) !== MAGIC ||
      dv.getUint16(4, true) !== VERSION)
    return; // 不识别的包：按协议约定直接丢弃
  const phase = dv.getFloat32(16, true);
  const disp = dv.getFloat32(20, true);
  const trackBin = dv.getInt32(24, true);
  const trackAmp = dv.getFloat32(28, true);
  const nWave = dv.getUint16(32, true);
  const nBins = dv.getUint16(34, true);
  if (buf.byteLength !== 36 + 4 * nWave + 4 * nBins) return;

  // TypedArray 视图整体拷贝一份（原 buffer 每帧丢弃，避免持有大对象）
  const iArr = new Int16Array(buf, 36, nWave).slice();
  const qArr = new Int16Array(buf, 36 + 2 * nWave, nWave).slice();
  const pArr = new Float32Array(buf.slice(36 + 4 * nWave, 36 + 4 * nWave + 4 * nBins));
  lastFrame = { iArr, qArr, pArr, trackBin };

  // 滚动缓冲写入
  const idx = writeIdx();
  tBuf[idx] = frameCount / meta.fps;
  phaseBuf[idx] = phase;
  dispBuf[idx] = disp;
  ampBuf[idx] = trackAmp;
  breathBuf[idx] = Number.isFinite(disp) ? biquad.step(disp) : 0;
  ++frameCount;
  n = Math.min(n + 1, cap);
  dirty = true;
}

function setStatus(text, ok) {
  $("status").textContent = text;
  $("status").className = ok ? "ok" : "";
}

function updateControls() {
  $("btn-conn").textContent = ws ? "断开" : "连接";
  $("btn-conn").classList.toggle("primary", !ws);
  $("btn-pause").disabled = !ws && !lastFrame; // 有过数据即可暂停查看
  $("btn-pause").textContent = paused ? "继续" : "暂停";
}

function connect() {
  const url = $("wsurl").value.trim();
  if (!url) return;
  manualClose = false;
  setStatus("连接中…", false);
  ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => { setStatus("已连接", true); updateControls(); };
  ws.onclose = () => {
    ws = null;
    updateControls();
    if (manualClose) {
      setStatus("已断开", false);
    } else {
      setStatus("已断开，1s 后重连…", false);
      setTimeout(() => { if (!manualClose && !ws) connect(); }, 1000); // 意外断开才自动重连
    }
  };
  ws.onerror = () => { if (ws) ws.close(); };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      try { const m = JSON.parse(ev.data); if (m.type === "meta") onMeta(m); } catch (_) {}
    } else {
      onFrame(ev.data);
    }
  };
  updateControls();
}

function disconnect() {
  manualClose = true; // 阻止自动重连
  if (ws) ws.close();
}

// 清空时间序列缓冲（ADC/距离谱是逐帧量，保留最新一帧）。
function clearBuffers() {
  n = 0;
  frameCount = 0;
  if (meta) biquad = makeBandpass(meta.fps, 0.1, 0.5); // 滤波器状态一并复位
  $("bpm").textContent = "--";
  dirty = true;
}

function initControls() {
  $("wsurl").value = WS_URL;
  $("btn-conn").onclick = () => (ws ? disconnect() : connect());
  $("btn-pause").onclick = () => { paused = !paused; dirty = true; updateControls(); };
  $("btn-clear").onclick = clearBuffers;
  $("wsurl").addEventListener("keydown", (e) => {
    if (e.key === "Enter") { disconnect(); connect(); }
  });
  updateControls();
}

// ---------------------------------------------------------------------------
// 渲染循环：按时间顺序展开环形缓冲 -> setData
// ---------------------------------------------------------------------------
function unroll(buf, len, start) {
  const out = new Array(len);
  for (let i = 0; i < len; ++i) out[i] = buf[(start + i) % cap];
  return out;
}

function render() {
  requestAnimationFrame(render);
  if (paused || !dirty || !meta || !lastFrame) return; // 暂停 = 冻结画面
  dirty = false;

  // ① 原始 ADC
  const xs = Array.from({ length: lastFrame.iArr.length }, (_, i) => i);
  chAdc.setData([xs, Array.from(lastFrame.iArr), Array.from(lastFrame.qArr)]);

  // ② 距离谱 + 跟踪 bin 标记
  const xr = Array.from({ length: lastFrame.pArr.length },
                        (_, i) => i * meta.rangeIdxToMeters);
  const marker = new Array(lastFrame.pArr.length).fill(null);
  if (lastFrame.trackBin >= 0 && lastFrame.trackBin < marker.length)
    marker[lastFrame.trackBin] = lastFrame.pArr[lastFrame.trackBin];
  chRange.setData([xr, Array.from(lastFrame.pArr), marker]);

  // ③④ 时间序列（环形缓冲展开）
  const len = Math.min(n, cap);
  const start = (writeIdx() - len + cap) % cap;
  const ts = unroll(tBuf, len, start);
  chPhase.setData([ts, unroll(phaseBuf, len, start), unroll(dispBuf, len, start)]);

  // 呼吸波形：trackAmp 门控 —— 不可靠样本置 null（曲线断开提示）
  const gate = GATE_RATIO * rollingMedianAmp();
  const breath = new Array(len);
  for (let i = 0; i < len; ++i) {
    const k = (start + i) % cap;
    breath[i] = ampBuf[k] < gate ? null : breathBuf[k];
  }
  chBreath.setData([ts, breath]);

  // 徽标
  $("trackbin").textContent = lastFrame.trackBin >= 0
    ? `${lastFrame.trackBin} (${(lastFrame.trackBin * meta.rangeIdxToMeters).toFixed(2)} m)` : "-";
  const bpm = estimateBpm();
  $("bpm").textContent = bpm === null ? "--" : bpm.toFixed(1);
}

initCharts();
initControls();
connect(); // 载入即自动连接；手动"断开"后不再自动重连
requestAnimationFrame(render);
