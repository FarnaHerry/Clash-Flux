[CmdletBinding()]
param(
    [switch]$BuildOnly,
    [string]$CMakeExecutable = '',
    [string[]]$DemoArguments = @()
)

$ErrorActionPreference = 'Stop'
$demoRoot = $PSScriptRoot
$repoRoot = Split-Path (Split-Path $demoRoot -Parent) -Parent
$buildRoot = Join-Path $demoRoot 'build-windows'
$frameworkRoot = Join-Path $repoRoot 'third_party/huxerui'

# Reuse the main build's CMake when Visual Studio bundles an older version.
if (-not $CMakeExecutable) {
    $mainCache = Join-Path $repoRoot 'build/CMakeCache.txt'
    if (Test-Path -LiteralPath $mainCache) {
        $cachedCommand = Select-String -LiteralPath $mainCache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$'
        if ($cachedCommand -and (Test-Path -LiteralPath $cachedCommand.Matches[0].Groups[1].Value)) {
            $CMakeExecutable = $cachedCommand.Matches[0].Groups[1].Value
        }
    }
    if (-not $CMakeExecutable) { $CMakeExecutable = 'cmake' }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Install Visual Studio C++ build tools, or run from Developer PowerShell.'
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ build tools were not found.' }
    & (Join-Path $installation 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}

& $CMakeExecutable -S $demoRoot -B $buildRoot -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DHUXERUI_HOME=$frameworkRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMakeExecutable --build $buildRoot --target box_cat_demo --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($BuildOnly) { exit 0 }

& (Join-Path $buildRoot 'box_cat_demo.exe') @DemoArguments | Out-Host
exit $LASTEXITCODE
