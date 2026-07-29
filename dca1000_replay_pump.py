#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dca1000_replay_pump.py — DCA1000 UDP 回放泵（无硬件测试用）

把离线 ADC bin 文件按 DCA1000 原始数据包格式回放到 UDP 端口，
配合 test_udp（test_udp_main.cpp）验证 “UDP 接收 → seqNum 重组 → DataParser 解析” 链路。

包格式（与 UdpReceiver.h 的 packet_t 一致，小端）：
    uint32  seqNum      # 从 1 开始递增
    uint48  byteCnt     # 本包 payload 之前累计发送的字节数（6 字节小端）
    uint8   payload[1456]

用法示例（500MB 数据集 = 2000 帧 × 262144 B/帧）：
    python3 dca1000_replay_pump.py \
        --bin data/datasets/hapy77--awr1843-adc-data/snapshots/master/adc_raw_data.bin \
        --host 127.0.0.1 --port 4098 --frame-bytes 262144 --fps 30 --max-frames 300
"""

import argparse
import socket
import struct
import sys
import time

PAYLOAD_SIZE = 1456  # PACKET_SIZE_DEFAULT(1466) - 10 字节包头


def main():
    ap = argparse.ArgumentParser(description="DCA1000 UDP replay pump")
    ap.add_argument("--bin", required=True, help="ADC 原始 bin 文件路径")
    ap.add_argument("--host", default="127.0.0.1", help="目标地址（test_udp 所在机器）")
    ap.add_argument("--port", type=int, default=4098, help="目标 UDP 端口")
    ap.add_argument("--frame-bytes", type=int, default=262144,
                    help="每帧字节数（仅用于发送节奏与进度统计）")
    ap.add_argument("--fps", type=float, default=30.0, help="回放帧率（帧/秒）")
    ap.add_argument("--loop", action="store_true", help="循环回放（Ctrl-C 停止）")
    ap.add_argument("--max-frames", type=int, default=0,
                    help="最多回放帧数（0 = 整个文件；--loop 时无限）")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
    dst = (args.host, args.port)

    byte_rate = args.fps * args.frame_bytes          # 目标字节速率
    pkt_interval = PAYLOAD_SIZE / byte_rate          # 每包间隔（秒）

    seq = 1                # DCA1000 seqNum 从 1 开始
    byte_cnt = 0           # 累计 payload 字节数
    frames_sent = 0
    t0 = time.monotonic()

    print(f"[pump] {args.bin} -> {args.host}:{args.port}, "
          f"frame={args.frame_bytes}B, fps={args.fps}, loop={args.loop}")

    try:
        while True:
            with open(args.bin, "rb") as f:
                while True:
                    payload = f.read(PAYLOAD_SIZE)
                    if not payload:
                        break  # 文件读尽
                    if len(payload) < PAYLOAD_SIZE:
                        payload += b"\x00" * \
                            (PAYLOAD_SIZE - len(payload))  # 尾包补零保持定长
                    # 4B seqNum + 6B byteCnt（小端 48bit）+ payload
                    header = struct.pack("<I", seq) + \
                        struct.pack("<Q", byte_cnt)[:6]
                    sock.sendto(header + payload, dst)
                    seq += 1
                    byte_cnt += PAYLOAD_SIZE

                    # 按目标速率整流（每 50 包校准一次，避免 sleep 精度问题）
                    if seq % 50 == 0:
                        target = t0 + (byte_cnt / byte_rate)
                        now = time.monotonic()
                        if target > now:
                            time.sleep(target - now)

                    new_frames = byte_cnt // args.frame_bytes
                    if new_frames > frames_sent:
                        frames_sent = new_frames
                        if frames_sent % 100 == 0:
                            elapsed = time.monotonic() - t0
                            print(f"[pump] 已发送 {frames_sent} 帧, "
                                  f"{seq - 1} 包, {elapsed:.1f}s")
                    if args.max_frames and frames_sent >= args.max_frames:
                        raise KeyboardInterrupt
            if not args.loop:
                break
    except KeyboardInterrupt:
        pass
    finally:
        elapsed = time.monotonic() - t0
        print(f"[pump] 结束: 共 {seq - 1} 包 / {byte_cnt} 字节 "
              f"≈ {frames_sent} 帧, 用时 {elapsed:.1f}s")
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
