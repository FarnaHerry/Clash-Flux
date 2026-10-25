# Windows 等价于 run.sh：检查二进制、设置 Intel Arc workaround、透传参数后启动
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$bin = $env:CLASHFLUX_BIN
if (-not $bin) {
    $runtimeDir = $null
    $cache = Join-Path $PSScriptRoot 'build\CMakeCache.txt'
    if (Test-Path -LiteralPath $cache -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $cache) {
            if ($line -match '^CMAKE_RUNTIME_OUTPUT_DIRECTORY:[^=]+=(.+)$') {
                $runtimeDir = $Matches[1]
                break
            }
        }
    }
    $bin = if ($runtimeDir) {
        Join-Path $runtimeDir 'clash-flux.exe'
    } else {
        Join-Path $PSScriptRoot 'build\clash-flux.exe'
    }
}
if (-not (Test-Path -LiteralPath $bin -PathType Leaf)) {
    Write-Error "binary not found ($bin) - run 'cmake --build build --target clash-flux' first"
    exit 1
}
if (-not $env:INTEL_FORCE_PROBE) { $env:INTEL_FORCE_PROBE = '1' }   # Intel Arc B390 iris DRI workaround

& $bin @args
exit $LASTEXITCODE
