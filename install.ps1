# install.ps1 — installs the agentvfs prebuilt to %LOCALAPPDATA%\Programs\agentvfs.
# Usage: iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex

[CmdletBinding()]
param(
    [string]$Version = $env:AGENTVFS_INSTALL_VERSION,
    [string]$Prefix  = $env:AGENTVFS_INSTALL_PREFIX,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$Repo = 'thustorage/ContextFS'
$UrlBase = "https://github.com/$Repo/releases/download"

if ($Help) {
    @'
Usage: install.ps1 [-Version <tag>] [-Prefix <dir>] [-Help]

Installs the agentvfs prebuilt to Prefix.
Default Prefix is %LOCALAPPDATA%\Programs\agentvfs (no admin).

Environment:
  AGENTVFS_INSTALL_VERSION   pin a release tag (e.g. v0.1.0)
  AGENTVFS_INSTALL_PREFIX    override install destination
'@ | Write-Output
    return
}

$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne 'X64') {
    throw "install.ps1: unsupported arch '$arch' — only windows-x86_64 has a prebuilt. Build from source."
}
$Tuple = 'windows-x86_64'

if (-not $Version) {
    try {
        $rel = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/latest"
        $Version = $rel.tag_name
    } catch {
        throw "install.ps1: could not resolve latest release; pin with -Version vX.Y.Z"
    }
}

if (-not $Prefix) { $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\agentvfs' }

$archive    = "agentvfs-$Version-$Tuple.zip"
$checksums  = "agentvfs-$Version-checksums.txt"
$work = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "agentvfs-install-$(Get-Random)") -Force

try {
    Invoke-WebRequest -UseBasicParsing -Uri "$UrlBase/$Version/$archive"   -OutFile (Join-Path $work $archive)
    Invoke-WebRequest -UseBasicParsing -Uri "$UrlBase/$Version/$checksums" -OutFile (Join-Path $work $checksums)

    $expected = (Get-Content (Join-Path $work $checksums) | Where-Object { $_ -like "*  $archive" } | Select-Object -First 1) -replace '\s.*',''
    if (-not $expected) { throw "install.ps1: $archive not in checksums file" }
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $work $archive)).Hash.ToLower()
    if ($expected.ToLower() -ne $actual) {
        throw "install.ps1: checksum mismatch for $archive`n  expected: $expected`n  actual:   $actual"
    }

    # Archive is expected to be flat (no nested dir); see release.yml packaging.
    Expand-Archive -Path (Join-Path $work $archive) -DestinationPath $work -Force
    if (-not (Test-Path (Join-Path $work 'agentvfs.exe'))) {
        throw "install.ps1: archive missing 'agentvfs.exe' — please report at https://github.com/thustorage/ContextFS/issues"
    }
    New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
    foreach ($f in 'agentvfs.exe','agentvfs-ctl.exe') {
        Copy-Item -Path (Join-Path $work $f) -Destination (Join-Path $Prefix $f) -Force
    }

    Write-Output ""
    Write-Output "Installed agentvfs $Version to $Prefix"
    Write-Output ""
    Write-Output "next:"
    Write-Output "  agentvfs.exe --source C:\some\dir --mountpoint Z:"
    Write-Output "  agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline"

    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    if ($userPath -notlike "*$Prefix*") {
        Write-Output ""
        Write-Output "  (add $Prefix to your PATH)"
    }
} finally {
    Remove-Item -Recurse -Force $work
}
