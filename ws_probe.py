#!/usr/bin/env python3
"""ws_probe.py — 最小 WebSocket 探针，验证实时显示链路真的在推帧。

用途：在没有浏览器（或 headless Jetson）时确认 `radar_capture_web` /
`radar_web_demo` 的 WebSocket 推送是否正常，并直接打印 wire 协议头
（见 src/web/WireProtocol.h）。

只依赖 Python 标准库，无需 pip 安装。

用法：
    python3 ws_probe.py [host] [port] [messages]
    默认：127.0.0.1 8765 3

输出示例：
    handshake OK
    text    {"type":"meta","version":1,"nWave":256,"nBins":256,...}
    binary  frame#0 magic=0x31574452 v1 flags=1 phase=-0.7363 disp=0.0000 nWave=256 nBins=256 bytes=2084
"""
import base64
import os
import socket
import struct
import sys

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
want = int(sys.argv[3]) if len(sys.argv) > 3 else 3

s = socket.create_connection((host, port), timeout=10)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(
    (
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    ).encode()
)
resp = s.recv(4096)
if b"101" not in resp.split(b"\r\n")[0]:
    print("HANDSHAKE-FAILED", resp[:120])
    sys.exit(1)
print("handshake OK")


def read_exact(n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("connection closed")
        buf += chunk
    return buf


for _ in range(want):
    hdr = read_exact(2)
    opcode = hdr[0] & 0x0F
    length = hdr[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(8))[0]
    payload = read_exact(length)
    if opcode == 0x02:  # binary：wire 协议 v1
        magic, version, flags = struct.unpack("<IHH", payload[:8])
        frame_seq = struct.unpack("<Q", payload[8:16])[0]
        phase = struct.unpack("<f", payload[16:20])[0]
        disp = struct.unpack("<f", payload[20:24])[0]
        n_wave, n_bins = struct.unpack("<HH", payload[32:36])
        print(
            f"binary  frame#{frame_seq} magic=0x{magic:08x} v{version} flags={flags} "
            f"phase={phase:.4f} disp={disp:.4f} nWave={n_wave} nBins={n_bins} "
            f"bytes={len(payload)}"
        )
    else:  # text：接入时下发的 meta JSON
        print(f"text    {payload[:120].decode(errors='replace')}")
s.close()
