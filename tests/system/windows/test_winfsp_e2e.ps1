$ErrorActionPreference = 'Stop'

$src = New-Item -ItemType Directory -Path "$env:TEMP\agentvfs-$([guid]::NewGuid())"
$store = "$($src.FullName)\.agentvfs-store"
$mount = "Z:"
Set-Content -Path "$($src.FullName)\hello.txt" -Value "world"

# Use an explicit pipe name and pass it to both the daemon and the
# ctl client via --pipe / --sock. main_windows.cpp's auto-derivation
# (FNV-1a on the absolute store path) is irrelevant here because we
# control both sides of the conversation. Earlier this script tried
# to mirror that hash in PowerShell and hit numeric-type overflow on
# the multiply-and-mask step — replacing it removes that whole class
# of bug from the test.
$pipe = "\\.\pipe\agentvfs-test-" + [guid]::NewGuid().ToString("N").Substring(0, 8)

$errLog = "$($src.FullName)\err.log"
$outLog = "$($src.FullName)\out.log"
$d = Start-Process .\build\Release\agentvfs.exe `
    -ArgumentList "--source",$src.FullName,"--mountpoint",$mount,`
                  "--store",$store,"--pipe",$pipe `
    -PassThru -NoNewWindow `
    -RedirectStandardOutput $outLog `
    -RedirectStandardError $errLog

function Dump-Diag($label) {
    Write-Host "=== $label ==="
    Write-Host "daemon HasExited=$($d.HasExited)$(if ($d.HasExited) { ' ExitCode='+$d.ExitCode })"
    Write-Host "--- stderr ---"
    Get-Content $errLog -ErrorAction SilentlyContinue | Write-Host
    Write-Host "--- stdout ---"
    Get-Content $outLog -ErrorAction SilentlyContinue | Write-Host
    Write-Host "--- mount root contents ---"
    Get-ChildItem "$mount\" -Force -ErrorAction SilentlyContinue |
        Select-Object Name, Length | Format-Table | Out-String | Write-Host
    Write-Host "=== end $label ==="
}

try {
    $deadline = (Get-Date).AddSeconds(30)
    do { Start-Sleep -Milliseconds 250 } until (
        (Test-Path "$mount\hello.txt") -or (Get-Date) -gt $deadline -or $d.HasExited)
    if (-not (Test-Path "$mount\hello.txt")) {
        Dump-Diag "mount-timeout"
        throw "mount timeout"
    }

    # Smoke: read, write, read.
    if ((Get-Content "$mount\hello.txt" -Raw).Trim() -ne "world") {
        throw "read mismatch"
    }
    Set-Content "$mount\new.txt" "hi"
    if ((Get-Content "$mount\new.txt" -Raw).Trim() -ne "hi") {
        throw "write/read round-trip failed"
    }

    # Checkpoint, edit, rollback.
    & .\build\Release\agentvfs-ctl.exe --sock $pipe checkpoint baseline
    if ($LASTEXITCODE -ne 0) { throw "checkpoint failed" }
    Set-Content "$mount\new.txt" "edited"
    & .\build\Release\agentvfs-ctl.exe --sock $pipe rollback baseline
    if ($LASTEXITCODE -ne 0) { throw "rollback failed" }
    $after = (Get-Content "$mount\new.txt" -Raw -ErrorAction SilentlyContinue)
    if ($after -and $after.Trim() -eq "edited") {
        throw "rollback didn't restore"
    }

    Write-Host "test_winfsp_e2e: OK"
} finally {
    if ($d -and -not $d.HasExited) { Stop-Process -Id $d.Id -Force }
    Remove-Item -Recurse -Force $src.FullName -ErrorAction SilentlyContinue
}
