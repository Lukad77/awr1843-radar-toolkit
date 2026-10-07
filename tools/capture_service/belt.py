"""Optional belt contract. No device dependency is required for radar-only runs."""
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol

@dataclass(frozen=True)
class SessionContext:
    directory: Path
    origin_monotonic_ns: int
    origin_utc_ns: int
    clock_ns: Callable[[], int]
    belt_options: dict = field(default_factory=dict)
    is_cancelled: Callable[[], bool] = lambda: False

class BeltAdapter(Protocol):
    """Methods must have bounded timeouts; start begins asynchronous sampling.

    Write raw bytes, chunk timestamps, samples and quality metadata.
    clock_ns() and radar's QPC timestamps share Windows' perf_counter epoch.
    Device timestamps must be retained separately; host receipt is NOT hardware
    acquisition time. stop must be idempotent and flush/close partial output.
    status returns {'error': '...'} on loss/failure; the whole session then stops.
    """
    def prepare(self, context: SessionContext) -> None: ...
    def start(self) -> None: ...
    def status(self) -> dict: ...
    def stop(self) -> dict: ...

# Importing the adapter does not import pySerial or open any port.
from hkh11c import Hkh11cDevice

BELT_ADAPTERS: dict[str, Callable[[], BeltAdapter]] = {'HKH-11C': Hkh11cDevice}
