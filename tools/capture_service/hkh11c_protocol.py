"""Streaming HKH-11C parser; no device or third-party dependency."""
from collections import deque
from dataclasses import dataclass

@dataclass(frozen=True)
class BeltFrame:
    raw_offset: int
    first_chunk: int
    last_chunk: int
    rx_ns: int
    raw: bytes

    @property
    def command(self):
        return self.raw[4]

    @property
    def value(self):
        return int.from_bytes(self.raw[5:7], 'big') if self.command == 0xA0 else None

class Hkh11cParser:
    LENGTHS = {0xA0: 5, 0xA1: 3, 0xA2: 7, 0xA3: 7, 0xA4: 3}

    def __init__(self):
        self.buffer = bytearray()
        self.offset = 0
        self.received = 0
        self.chunks = deque()
        self.checksum_errors = 0
        self.length_errors = 0
        self.unknown_commands = 0
        self.skipped_bytes = 0

    def _consume(self, count, skipped=False):
        del self.buffer[:count]
        self.offset += count
        if skipped:
            self.skipped_bytes += count
        while self.chunks and self.chunks[0][1] <= self.offset:
            self.chunks.popleft()

    def feed(self, data: bytes, chunk_id: int, rx_ns: int):
        if not data:
            return []
        self.chunks.append((self.received, self.received+len(data), chunk_id, rx_ns))
        self.received += len(data)
        self.buffer.extend(data)
        frames = []
        while self.buffer:
            head = self.buffer.find(b'\xff\xcc')
            if head < 0:
                keep = 1 if self.buffer[-1] == 0xFF else 0
                self._consume(len(self.buffer)-keep, skipped=True)
                break
            if head:
                self._consume(head, skipped=True)
            if len(self.buffer) < 3:
                break
            length = self.buffer[2]
            if length not in (3, 5, 7):
                self.length_errors += 1
                self._consume(1, skipped=True)
                continue
            if len(self.buffer) < 5:
                break
            cmd = self.buffer[4]
            if cmd not in self.LENGTHS:
                self.unknown_commands += 1
                self._consume(1, skipped=True)
                continue
            if self.LENGTHS[cmd] != length:
                self.length_errors += 1
                self._consume(1, skipped=True)
                continue
            total = length+2
            if len(self.buffer) < total:
                break
            raw = bytes(self.buffer[:total])
            if (raw[2]+sum(raw[4:])) & 255 != raw[3]:
                self.checksum_errors += 1
                self._consume(1, skipped=True)
                continue
            first = self.chunks[0]
            last = next(c for c in self.chunks if c[0] <= self.offset+total-1 < c[1])
            frames.append(BeltFrame(self.offset, first[2], last[2], last[3], raw))
            self._consume(total)
        return frames

    def statistics(self):
        return dict(checksum_errors=self.checksum_errors, length_errors=self.length_errors,
                    unknown_commands=self.unknown_commands, skipped_bytes=self.skipped_bytes,
                    incomplete_tail_bytes=len(self.buffer), raw_bytes=self.received)

def command_bytes(command, parameter=None):
    params = b'' if parameter is None else bytes([parameter])
    length = 3+len(params)
    return bytes([0xFF, 0xCC, length, (length+command+sum(params)) & 255, command])+params
