# Windows 等价于 run.sh：检查二进制、设置 Intel Arc workaround、透传参数后启动
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$bin = if ($env:CLASHFLUX_BIN) { $env:CLASHFLUX_BIN } else { 'build\clash-flux.exe' }
if (-not (Test-Path $bin)) {
    Write-Error "binary not found ($bin) - run `cmake -B build -G Ninja; cmake --build build` first"
    exit 1
}
if (-not $env:INTEL_FORCE_PROBE) { $env:INTEL_FORCE_PROBE = '1' }   # Intel Arc B390 iris DRI workaround

& $bin @args
exit $LASTEXITCODE
