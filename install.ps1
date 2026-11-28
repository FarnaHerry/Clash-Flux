[CmdletBinding()]
param(
    [string]$Version,
    [string]$Prefix,
    [string]$Archive,
    [string]$Sha256,
    [switch]$Portable,
    [switch]$Passive,
    [switch]$Quiet,
    [switch]$DesktopShortcut,
    [switch]$Yes,
    [switch]$Uninstall,
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepositoryUrl = "https://github.com/FarnaHerry/Clash-Flux"
$ApiUrl = "https://api.github.com/repos/FarnaHerry/Clash-Flux"

function Show-Usage {
    Write-Host @"
Usage:
  install.ps1 [-Version <version>] [-Passive] [-Quiet] [-Prefix <path>] [-DesktopShortcut] [-Yes]
  install.ps1 -Portable [-Version <version>] [-Prefix <path>] [-DesktopShortcut] [-Yes]
  install.ps1 [-Archive <path>] [-Sha256 <hex>] [-Portable] [-Prefix <path>]
  install.ps1 -Uninstall [-Portable] [-Prefix <path>] [-Passive] [-Quiet] [-Yes]

Examples:
  irm https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.ps1 | iex
  & ([scriptblock]::Create((irm https://github.com/FarnaHerry/Clash-Flux/releases/latest/download/install.ps1))) -Passive
"@
}

function Fail([string]$Message) {
    throw "Clash-Flux installer: $Message"
}

function Get-AbsolutePath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

function Test-ReparsePoint([string]$Path) {
    $Item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    return $Item -and (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Assert-SafePrefix([string]$Path) {
    $Root = [System.IO.Path]::GetPathRoot($Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or
        $Path.Equals($Root, [System.StringComparison]::OrdinalIgnoreCase) -or
        $Path.Equals($env:USERPROFILE, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "unsafe installation prefix: $Path"
    }
    if (Test-ReparsePoint $Path) {
        Fail "installation prefix must not be a link: $Path"
    }
}

function Test-PortableInstall([string]$Root) {
    return (Test-Path -LiteralPath (Join-Path $Root "clash-flux.exe") -PathType Leaf)
}

function Confirm-Action([string]$Description) {
    Write-Host $Description
    if ($Yes) {
        return
    }
    $Answer = Read-Host "Continue? [y/N]"
    if ($Answer -notmatch "^(?i:y|yes)$") {
        Fail "cancelled"
    }
}

function Add-UserPathEntry([string]$BinDirectory) {
    $OldPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $Entries = [System.Collections.Generic.List[string]]::new()
    if ($OldPath) {
        foreach ($Entry in $OldPath.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
            if (-not $Entry.Equals($BinDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
                $Entries.Add($Entry)
            }
        }
    }
    $Entries.Add($BinDirectory)
    [Environment]::SetEnvironmentVariable("Path", ($Entries -join ";"), "User")
}

function Remove-UserPathEntry([string]$BinDirectory) {
    $OldPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (-not $OldPath) {
        return
    }
    $Entries = [System.Collections.Generic.List[string]]::new()
    foreach ($Entry in $OldPath.Split(";", [System.StringSplitOptions]::RemoveEmptyEntries)) {
        if (-not $Entry.Equals($BinDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
            $Entries.Add($Entry)
        }
    }
    [Environment]::SetEnvironmentVariable("Path", ($Entries -join ";"), "User")
}

function New-AppShortcut([string]$ShortcutPath, [string]$TargetExe, [string]$WorkingDir) {
    $Shell = New-Object -ComObject WScript.Shell
    $Shortcut = $Shell.CreateShortcut($ShortcutPath)
    $Shortcut.TargetPath = $TargetExe
    $Shortcut.WorkingDirectory = $WorkingDir
    $Shortcut.IconLocation = "$TargetExe,0"
    $Shortcut.Save()
}

function Get-Sha256FromSums([string]$SumsPath, [string]$AssetName) {
    if (-not (Test-Path -LiteralPath $SumsPath -PathType Leaf)) {
        return $null
    }
    foreach ($Line in (Get-Content -LiteralPath $SumsPath)) {
        $Trimmed = $Line.Trim()
        if ($Trimmed -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            $Hash = $Matches[1]
            $FileName = [System.IO.Path]::GetFileName($Matches[2].Trim())
            if ($FileName -eq $AssetName) {
                return $Hash
            }
        }
    }
    return $null
}

function Verify-PackageSha256([string]$FilePath, [string]$ExpectedHex, [string]$Label) {
    if (-not $ExpectedHex -or $ExpectedHex -notmatch '^[0-9a-fA-F]{64}$') {
        Fail "missing or invalid SHA-256 checksum for $Label"
    }
    $Actual = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash
    if (-not $Actual.Equals($ExpectedHex, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail "SHA-256 checksum mismatch for $Label (expected $ExpectedHex, got $Actual)"
    }
    Write-Host "Clash-Flux installer: verified SHA-256 for $Label"
}

function Find-BurnUninstallEntry {
    $Roots = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )
    foreach ($Root in $Roots) {
        $Match = Get-ItemProperty -Path $Root -ErrorAction SilentlyContinue | Where-Object {
            $_.PSObject.Properties["DisplayName"] -and
            $_.DisplayName -eq "Clash-Flux" -and
            $_.PSObject.Properties["BundleVersion"] -and
            ($_.PSObject.Properties["UninstallString"] -or $_.PSObject.Properties["QuietUninstallString"])
        } | Select-Object -First 1
        if ($Match) {
            return $Match
        }
    }
    return $null
}

if ($Help) {
    Show-Usage
    exit 0
}

if ($Uninstall -and ($Version -or $Archive -or $Sha256)) {
    Fail "-Uninstall cannot be combined with -Version, -Archive, or -Sha256"
}
if ($Version -and $Archive) {
    Fail "-Version and -Archive cannot be combined"
}
if ($Quiet -and $Portable) {
    # Quiet/Passive only alter the Burn .exe installer UX; harmless if omitted in portable mode.
}

$Architecture = $(if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }).ToLowerInvariant()
if ($Architecture -notin @("amd64", "x64", "x86_64")) {
    Fail "Windows releases currently support x86_64 hosts only"
}
$HostArchitecture = "x86_64"

if ($Prefix) {
    $Prefix = Get-AbsolutePath $Prefix
    Assert-SafePrefix $Prefix
} elseif ($Portable) {
    if (-not $env:LOCALAPPDATA) {
        Fail "LOCALAPPDATA is not configured"
    }
    $Prefix = Get-AbsolutePath (Join-Path $env:LOCALAPPDATA "Clash-Flux")
    Assert-SafePrefix $Prefix
}

if ($Uninstall) {
    if ($Portable) {
        if (-not (Test-PortableInstall $Prefix)) {
            Fail "refusing to uninstall: Clash-Flux executable not found at $Prefix"
        }
        Confirm-Action "Uninstall portable Clash-Flux from $Prefix (user configuration is preserved)."
        try {
            $Lock = [IO.File]::Open("$Prefix.clash-flux-lock", [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        } catch {
            Fail "another installer may be using $Prefix"
        }
        try {
            Remove-UserPathEntry $Prefix
            $StartShortcut = Join-Path ([Environment]::GetFolderPath("Programs")) "Clash-Flux.lnk"
            if (Test-Path -LiteralPath $StartShortcut) { Remove-Item -LiteralPath $StartShortcut -Force }
            $DesktopLnk = Join-Path ([Environment]::GetFolderPath("DesktopDirectory")) "Clash-Flux.lnk"
            if (Test-Path -LiteralPath $DesktopLnk) { Remove-Item -LiteralPath $DesktopLnk -Force }
            Remove-Item -LiteralPath $Prefix -Recurse -Force
        } finally {
            $Lock.Dispose()
            Remove-Item -LiteralPath "$Prefix.clash-flux-lock" -ErrorAction SilentlyContinue
        }
        Write-Host "Clash-Flux portable installation removed from $Prefix"
        exit 0
    }

    $Entry = Find-BurnUninstallEntry
    if (-not $Entry) {
        Fail "Clash-Flux installer registration not found; pass -Portable if installed as a portable archive"
    }
    Confirm-Action "Uninstall Clash-Flux ($($Entry.DisplayVersion)); user configuration is preserved."
    $RawCmd = if ($Quiet -and $Entry.PSObject.Properties["QuietUninstallString"]) {
        $Entry.QuietUninstallString
    } else {
        $Entry.UninstallString
    }
    if ($RawCmd -match '^\s*"([^"]+)"\s*(.*)$') {
        $UninstallExe = $Matches[1]
        $UninstallArgs = @($Matches[2] -split '\s+' | Where-Object { $_ })
    } else {
        $Parts = @($RawCmd.Trim() -split '\s+' | Where-Object { $_ })
        $UninstallExe = $Parts[0]
        $UninstallArgs = if ($Parts.Count -gt 1) { $Parts[1..($Parts.Count - 1)] } else { @() }
    }
    if ($UninstallArgs -notcontains "/uninstall") {
        $UninstallArgs += "/uninstall"
    }
    if ($Quiet -and $UninstallArgs -notcontains "/quiet") {
        $UninstallArgs += @("/quiet", "/norestart")
    } elseif ($Passive -and $UninstallArgs -notcontains "/passive") {
        $UninstallArgs += @("/passive", "/norestart")
    }
    $Proc = Start-Process -FilePath $UninstallExe -ArgumentList $UninstallArgs -Wait -PassThru
    if ($Proc.ExitCode -eq 1602) {
        Write-Host "Clash-Flux uninstall cancelled by user."
        exit 0
    }
    if ($Proc.ExitCode -notin @(0, 3010)) {
        Fail "uninstaller failed with exit code $($Proc.ExitCode)"
    }
    Write-Host "Clash-Flux uninstalled successfully."
    exit 0
}

$ReleaseTag = $null
if (-not $Archive) {
    if (-not $Version) {
        try {
            $Response = (Invoke-WebRequest -Uri "$RepositoryUrl/releases/latest" -Method Head -UseBasicParsing).BaseResponse
            $ReleaseUri = if ($Response -is [System.Net.HttpWebResponse]) {
                $Response.ResponseUri
            } else {
                $Response.RequestMessage.RequestUri
            }
        } catch {
            Fail "failed to resolve the latest Clash-Flux release: $_"
        }
        $TagPrefix = [regex]::Escape("$RepositoryUrl/releases/tag/")
        if (-not $ReleaseUri -or $ReleaseUri.AbsoluteUri -cnotmatch "^$TagPrefix(v[0-9A-Za-z._-]+)$") {
            Fail "latest release did not redirect to a valid tag: $ReleaseUri"
        }
        $ReleaseTag = $Matches[1]
        $Version = $ReleaseTag.Substring(1)
    } else {
        $Version = $Version.TrimStart("v")
        $ReleaseTag = "v$Version"
    }
    if ($Version -notmatch "^[0-9A-Za-z._-]+$") {
        Fail "invalid version: $Version"
    }
    $Extension = if ($Portable) { "zip" } else { "exe" }
    $AssetName = "clash-flux-$Version-windows-$HostArchitecture.$Extension"
    $AssetUrl = "$RepositoryUrl/releases/download/$ReleaseTag/$AssetName"
} else {
    $Archive = Get-AbsolutePath $Archive
    if (-not (Test-Path -LiteralPath $Archive -PathType Leaf)) {
        Fail "archive does not exist: $Archive"
    }
    $AssetName = Split-Path -Leaf $Archive
    if ($AssetName -match '\.zip$') {
        if (-not $Portable) {
            $Portable = [switch]::Present
            if (-not $Prefix) {
                $Prefix = Get-AbsolutePath (Join-Path $env:LOCALAPPDATA "Clash-Flux")
                Assert-SafePrefix $Prefix
            }
        }
    } elseif ($AssetName -match '\.exe$') {
        if ($Portable) {
            Fail "-Portable requires a .zip archive"
        }
    } else {
        Fail "unsupported package extension: $AssetName (expected .exe or .zip)"
    }
}

$TemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("clash-flux-install-" + [guid]::NewGuid())
$StagingDirectory = $null
$BackupDirectory = $null
$InstallLock = $null
New-Item -ItemType Directory -Path $TemporaryDirectory | Out-Null

try {
    $PackagePath = $Archive
    if (-not $PackagePath) {
        $PackagePath = Join-Path $TemporaryDirectory $AssetName
        Write-Host "Clash-Flux installer: downloading $AssetName ($ReleaseTag)"
        Invoke-WebRequest -Uri $AssetUrl -OutFile $PackagePath -UseBasicParsing

        if (-not $Sha256) {
            $SumsPath = Join-Path $TemporaryDirectory "SHA256SUMS.txt"
            try {
                Invoke-WebRequest -Uri "$RepositoryUrl/releases/download/$ReleaseTag/SHA256SUMS.txt" -OutFile $SumsPath -UseBasicParsing
                $Sha256 = Get-Sha256FromSums $SumsPath $AssetName
            } catch {
                $Sha256 = $null
            }
        }
        if (-not $Sha256) {
            try {
                $ReleaseInfo = Invoke-RestMethod -Uri "$ApiUrl/releases/tags/$ReleaseTag" -UseBasicParsing
                foreach ($Asset in $ReleaseInfo.assets) {
                    if ($Asset.name -eq $AssetName -and $Asset.digest -match '^sha256:([0-9a-fA-F]{64})$') {
                        $Sha256 = $Matches[1]
                        break
                    }
                }
            } catch {
                $Sha256 = $null
            }
        }
    } else {
        if (-not $Sha256) {
            $SidecarSha = "$PackagePath.sha256"
            if (Test-Path -LiteralPath $SidecarSha -PathType Leaf) {
                $Parts = ((Get-Content -LiteralPath $SidecarSha -TotalCount 1).Trim() -split "\s+")
                if ($Parts.Count -ge 1) { $Sha256 = $Parts[0] }
            }
            if (-not $Sha256) {
                $Sha256 = Get-Sha256FromSums (Join-Path (Split-Path -Parent $PackagePath) "SHA256SUMS.txt") $AssetName
            }
        }
    }

    Verify-PackageSha256 $PackagePath $Sha256 $AssetName

    if (-not $Portable) {
        $InstallerArgs = [System.Collections.Generic.List[string]]::new()
        if ($Quiet) {
            $InstallerArgs.Add("/quiet")
            $InstallerArgs.Add("/norestart")
        } elseif ($Passive) {
            $InstallerArgs.Add("/passive")
            $InstallerArgs.Add("/norestart")
        }
        if ($Prefix) {
            $InstallerArgs.Add("InstallFolder=`"$Prefix`"")
        }
        if ($DesktopShortcut) {
            $InstallerArgs.Add("CreateDesktopShortcut=1")
        }

        Write-Host "Clash-Flux installer: launching $AssetName"
        if ($InstallerArgs.Count -gt 0) {
            $Proc = Start-Process -FilePath $PackagePath -ArgumentList $InstallerArgs -Wait -PassThru
        } else {
            $Proc = Start-Process -FilePath $PackagePath -Wait -PassThru
        }
        if ($Proc.ExitCode -eq 1602) {
            Write-Host "Clash-Flux installation cancelled by user."
            exit 0
        }
        if ($Proc.ExitCode -eq 3010) {
            Write-Host "Clash-Flux installed successfully. A system restart may be required."
            exit 0
        }
        if ($Proc.ExitCode -ne 0) {
            Fail "installer exited with code $($Proc.ExitCode)"
        }
        Write-Host "Clash-Flux installed successfully."
        exit 0
    }

    if ((Test-Path -LiteralPath $Prefix) -and -not (Test-PortableInstall $Prefix)) {
        Fail "installation prefix exists and is not a portable Clash-Flux directory: $Prefix"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $Zip = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        foreach ($Entry in $Zip.Entries) {
            $Name = $Entry.FullName.Replace('\', '/')
            if ($Name.StartsWith("/", [StringComparison]::Ordinal) -or
                $Name -match '(^|/)\.\.(/|$)|:' -or
                (($Entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) {
                Fail "archive contains an invalid path or symbolic link: $Name"
            }
        }
    } finally {
        $Zip.Dispose()
    }

    $ExtractDir = Join-Path $TemporaryDirectory "extract"
    Expand-Archive -LiteralPath $PackagePath -DestinationPath $ExtractDir
    $PayloadDir = $null
    if (Test-Path -LiteralPath (Join-Path $ExtractDir "clash-flux.exe") -PathType Leaf) {
        $PayloadDir = $ExtractDir
    } else {
        $ChildDirs = @(Get-ChildItem -LiteralPath $ExtractDir -Directory | Where-Object {
            Test-Path -LiteralPath (Join-Path $_.FullName "clash-flux.exe") -PathType Leaf
        })
        if ($ChildDirs.Count -eq 1) {
            $PayloadDir = $ChildDirs[0].FullName
        }
    }
    if (-not $PayloadDir -or -not (Test-Path -LiteralPath (Join-Path $PayloadDir "huxerui.dll") -PathType Leaf)) {
        Fail "archive does not contain a complete portable Clash-Flux runtime"
    }

    $Parent = Split-Path -Parent $Prefix
    New-Item -ItemType Directory -Path $Parent -Force | Out-Null
    try {
        $InstallLock = [IO.File]::Open("$Prefix.clash-flux-lock", [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    } catch {
        Fail "another installer is modifying $Prefix"
    }

    $StagingDirectory = Join-Path $Parent (".clash-flux-stage-" + [guid]::NewGuid())
    Move-Item -LiteralPath $PayloadDir -Destination $StagingDirectory
    if (Test-Path -LiteralPath $Prefix) {
        $BackupDirectory = Join-Path $Parent (".clash-flux-backup-" + [guid]::NewGuid())
        Move-Item -LiteralPath $Prefix -Destination $BackupDirectory
    }
    try {
        Move-Item -LiteralPath $StagingDirectory -Destination $Prefix
        $StagingDirectory = $null
        Add-UserPathEntry $Prefix
        $InstalledExe = Join-Path $Prefix "clash-flux.exe"
        $StartShortcut = Join-Path ([Environment]::GetFolderPath("Programs")) "Clash-Flux.lnk"
        New-AppShortcut $StartShortcut $InstalledExe $Prefix
        if ($DesktopShortcut) {
            $DesktopLnk = Join-Path ([Environment]::GetFolderPath("DesktopDirectory")) "Clash-Flux.lnk"
            New-AppShortcut $DesktopLnk $InstalledExe $Prefix
        }
    } catch {
        if (Test-Path -LiteralPath $Prefix) {
            Remove-Item -LiteralPath $Prefix -Recurse -Force -ErrorAction SilentlyContinue
        }
        if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
            Move-Item -LiteralPath $BackupDirectory -Destination $Prefix
            $BackupDirectory = $null
        }
        throw
    }

    if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
        Remove-Item -LiteralPath $BackupDirectory -Recurse -Force
        $BackupDirectory = $null
    }
    Write-Host "Clash-Flux portable edition installed at $Prefix"
} finally {
    if ($InstallLock) {
        $InstallLock.Dispose()
        Remove-Item -LiteralPath "$Prefix.clash-flux-lock" -ErrorAction SilentlyContinue
    }
    if ($StagingDirectory -and (Test-Path -LiteralPath $StagingDirectory)) {
        Remove-Item -LiteralPath $StagingDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($BackupDirectory -and (Test-Path -LiteralPath $BackupDirectory)) {
        Write-Warning "Previous installation remains at $BackupDirectory"
    }
    if (Test-Path -LiteralPath $TemporaryDirectory) {
        Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force -ErrorAction SilentlyContinue
    }
}
