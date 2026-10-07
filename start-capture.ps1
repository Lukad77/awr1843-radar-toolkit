param([int]$Port = 8090)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (-not (Test-Path build\radar_capture.exe)) {
    throw '请先执行 cmake -S . -B build -G Ninja，再执行 cmake --build build -j 6。'
}
if (Test-Path .venv\Scripts\python.exe) {
    & .\.venv\Scripts\python.exe tools\capture_service\server.py --port $Port
} else {
    python tools\capture_service\server.py --port $Port
}
