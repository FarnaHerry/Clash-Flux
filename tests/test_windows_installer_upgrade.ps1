param(
    [Parameter(Mandatory)][string] $Bundle,
    [Parameter(Mandatory)][string] $Wix,
    [Parameter(Mandatory)][string] $PackageSource,
    [Parameter(Mandatory)][string] $BundleSource,
    [Parameter(Mandatory)][string] $InstallerStaging,
    [Parameter(Mandatory)][string] $InstallerPayloads,
    [Parameter(Mandatory)][string] $Staging,
    [Parameter(Mandatory)][string] $Version
)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This test installs and removes Clash-Flux on a disposable GitHub-hosted Windows runner.'
}

function Invoke-Installer([string] $File, [string[]] $Arguments, [int] $Expected = 0) {
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne $Expected) {
        throw "$(Split-Path $File -Leaf) returned $($process.ExitCode), expected $Expected."
    }
}

function Get-ClashFluxRegistrations {
    Get-ItemProperty 'HKLM:/Software/Microsoft/Windows/CurrentVersion/Uninstall/*' |
        Where-Object { $_.DisplayName -eq 'Clash-Flux' -and $_.Publisher -eq 'dev.farna.clashflux' }
}

function Assert-AppVersion([string] $Expected) {
    $output = & $app --version
    if ($LASTEXITCODE -ne 0 -or $output -notmatch "v$([regex]::Escape($Expected))\b") {
        throw "Expected installed Clash-Flux v$Expected, got '$output'."
    }
}

$app = Join-Path $env:ProgramFiles 'Clash-Flux/clash-flux.exe'
$data = Join-Path $env:APPDATA 'clash-flux'
if ((Get-ClashFluxRegistrations) -or (Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath $data)) {
    throw 'Clash-Flux is already present on the runner.'
}

$scratch = Join-Path $env:RUNNER_TEMP "clashflux-installer-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $scratch | Out-Null
$publishedInstaller = Join-Path $scratch 'v0.3.68-published.exe'
Invoke-WebRequest 'https://github.com/FarnaHerry/Clash-Flux/releases/download/v0.3.68/clash-flux-Setup-0.3.68.exe' `
    -MaximumRetryCount 3 -OutFile $publishedInstaller
if ((Get-FileHash -LiteralPath $publishedInstaller).Hash -ne '4b3630eecb3d64cbdcdddb9d2aaef98b4c0e1f8381799d328a3bc0965e4576f6') {
    throw 'The legacy installer digest does not match.'
}

# The published bootstrapper passes a null HWND in quiet mode. Wrap the original
# MSI with the current quiet-capable bootstrapper and the legacy Burn identity.
$legacyPayloads = Join-Path $scratch 'legacy-payloads'
& $Wix burn extract $publishedInstaller -o $legacyPayloads -oba (Join-Path $scratch 'legacy-bootstrapper')
if ($LASTEXITCODE -ne 0) { throw 'Legacy MSI extraction failed.' }
$legacyMsi = @(Get-ChildItem -LiteralPath $legacyPayloads -Recurse -File -Filter '*.msi')
if ($legacyMsi.Count -ne 1) { throw 'Expected one legacy MSI in the published bundle.' }
$legacyBundleSource = Join-Path $scratch 'LegacyBundle.wxs'
[xml] $legacyDocument = Get-Content -LiteralPath $BundleSource -Raw
$legacyDocument.Wix.Bundle.SetAttribute('Version', '0.3.68')
$legacyDocument.Wix.Bundle.SetAttribute('UpgradeCode', '6D6BD18F-EDA4-5D11-BA5A-DB11FB4C6A69')
$legacyDocument.Wix.Bundle.Chain.MsiPackage.SetAttribute('SourceFile', $legacyMsi[0].FullName)
$legacyDocument.Save($legacyBundleSource)
$old = Join-Path $scratch 'clash-flux-Setup-0.3.68.exe'
& $Wix build $legacyBundleSource $InstallerPayloads -arch x64 -bindpath "Installer=$InstallerStaging" -out $old
if ($LASTEXITCODE -ne 0) { throw 'Legacy test bundle build failed.' }

# Fail only the test copy after RemoveExistingProducts has run inside the MSI transaction.
$rollbackSource = Join-Path $scratch 'Rollback.wxs'
$rollbackMsi = Join-Path $scratch 'Rollback.msi'
$sequence = @'
    <CustomAction Id="FailInstallerValidation" Error="Intentional Clash-Flux rollback test failure" />
    <InstallExecuteSequence>
      <Custom Action="FailInstallerValidation" After="RemoveExistingProducts" Condition="WIX_UPGRADE_DETECTED" />
'@
$source = Get-Content -LiteralPath $PackageSource -Raw
if (-not $source.Contains('<InstallExecuteSequence>')) { throw 'Could not instrument the MSI transaction.' }
$source.Replace('    <InstallExecuteSequence>', $sequence) | Set-Content -LiteralPath $rollbackSource
& $Wix build $rollbackSource -arch x64 -bindpath "Application=$Staging" -out $rollbackMsi
if ($LASTEXITCODE -ne 0) { throw 'Rollback test MSI build failed.' }

$installLog = Join-Path $scratch 'legacy-install.log'
Invoke-Installer $old @('-quiet', '-norestart', '-log', "`"$installLog`"")
Assert-AppVersion '0.3.68'
$legacy = @(Get-ClashFluxRegistrations | Where-Object { $_.BundleCachePath })
if ($legacy.Count -ne 1 -or -not (Test-Path -LiteralPath $legacy[0].BundleCachePath)) {
    throw 'Legacy Burn did not register its cached installer.'
}
$legacy = $legacy[0]

New-Item -ItemType Directory -Path $data | Out-Null
$marker = Join-Path $data 'installer-test.txt'
Set-Content -LiteralPath $marker -Value 'installer data preservation'
$markerHash = (Get-FileHash -LiteralPath $marker).Hash

$rollbackLog = Join-Path $scratch 'rollback.log'
Invoke-Installer "$env:SystemRoot/System32/msiexec.exe" `
    @('/i', "`"$rollbackMsi`"", '/qn', '/norestart', '/l*v', "`"$rollbackLog`"") 1603
if (-not (Select-String -LiteralPath $rollbackLog -SimpleMatch 'Intentional Clash-Flux rollback test failure')) {
    throw 'The MSI failed before its intentional rollback point.'
}
Assert-AppVersion '0.3.68'
if (-not (Test-Path -LiteralPath $legacy.PSPath) -or -not (Test-Path -LiteralPath $legacy.BundleCachePath)) {
    throw 'Rollback did not preserve the legacy bundle.'
}
Write-Host 'MSI rollback restored v0.3.68 and preserved its Burn registration and cache.'

$upgradeLog = Join-Path $scratch 'upgrade.log'
Invoke-Installer $Bundle @('-quiet', '-norestart', '-log', "`"$upgradeLog`"")
Assert-AppVersion $Version
if ((Test-Path -LiteralPath $legacy.PSPath) -or (Test-Path -LiteralPath $legacy.BundleCachePath)) {
    throw 'Major upgrade left the legacy Burn registration or cached installer behind.'
}
Write-Host "Burn major upgrade from v0.3.68 to v$Version removed the legacy registration and cache."

$uninstallLog = Join-Path $scratch 'uninstall.log'
Invoke-Installer $Bundle @('-uninstall', '-quiet', '-norestart', '-log', "`"$uninstallLog`"")
if ((Get-ClashFluxRegistrations) -or (Test-Path -LiteralPath $app)) {
    throw 'Uninstall left installed files or registrations behind.'
}
if ((Get-FileHash -LiteralPath $marker).Hash -ne $markerHash) { throw 'Installer changed user data.' }
Write-Host 'Silent uninstall removed the product and preserved user data.'
