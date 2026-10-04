# Shared helpers for the gate scripts (bench.ps1, kill_test.ps1, soak.ps1).
# Dot-source: . "$PSScriptRoot\common.ps1"

$ErrorActionPreference = 'Stop'
$script:Repo = (Resolve-Path "$PSScriptRoot\..\..").Path

function Get-K6Path([string]$Explicit) {
    if ($Explicit -and (Test-Path $Explicit)) { return $Explicit }
    $cmd = Get-Command k6 -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in @("$env:ProgramFiles\k6\k6.exe", "$env:LOCALAPPDATA\Microsoft\WinGet\Links\k6.exe")) {
        if (Test-Path $p) { return $p }
    }
    throw "k6 not found; pass -K6 <path to k6.exe>"
}

function Get-BuildDir([string]$Preset) { return Join-Path $script:Repo "build\$Preset" }

function New-RunDir([string]$Name) {
    $dir = Join-Path $script:Repo ("build\gate\{0}-{1}" -f $Name, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force $dir | Out-Null
    return $dir
}

# Mock backend as a separate process (so it can be hard-killed). Returns the Process.
function Start-Mock([string]$Preset, [int]$Port, [string]$Id, [string[]]$Extra = @(), [string]$LogDir) {
    $exe = Join-Path (Get-BuildDir $Preset) 'tools\mock_backend\mock_backend.exe'
    $argList = @('--port', $Port, '--id', $Id) + $Extra
    $log = Join-Path $LogDir "mock-$Id-$Port-$(Get-Random).log"
    $p = Start-Process $exe -ArgumentList $argList -PassThru -WindowStyle Hidden -RedirectStandardOutput $log
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $log) -and (Get-Content $log -Raw -ErrorAction SilentlyContinue) -match 'listening on') { return $p }
        if ($p.HasExited) { throw "mock $Id on $Port exited at start (port in use?)" }
        Start-Sleep -Milliseconds 50
    }
    throw "mock $Id on $Port did not start"
}

# Proxy (lb_console) with stdin/stdout redirected, so scripts can send commands.
function Start-Proxy([string]$Preset, [string]$Config) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = Join-Path (Get-BuildDir $Preset) 'tools\lb_console\lb_console.exe'
    $psi.Arguments = "--config `"$Config`""
    $psi.WorkingDirectory = $script:Repo   # event log paths in configs are relative to the repo
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    # Windows PowerShell 5.1 (.NET Framework) has no StandardInputEncoding: stdin uses the
    # console input encoding, which may add a UTF-8 BOM; lb_console strips it.
    $p = [System.Diagnostics.Process]::Start($psi)
    $line = $p.StandardOutput.ReadLine()
    if ($line -notmatch 'listening on') { throw "proxy did not start: $line" }
    Write-Host "  $line"
    return $p
}

function Save-ProxyMetrics($Proxy, [string]$Path) {
    if (Test-Path $Path) { Remove-Item $Path }
    $Proxy.StandardInput.WriteLine("metrics $Path")
    $Proxy.StandardInput.Flush()
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path $Path) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    Start-Sleep -Milliseconds 100
    return Get-Content $Path -Raw | ConvertFrom-Json
}

function Stop-Proxy($Proxy) {
    if ($Proxy.HasExited) { return }
    $Proxy.StandardInput.WriteLine('quit')
    $Proxy.StandardInput.Flush()
    if (-not $Proxy.WaitForExit(30000)) { $Proxy.Kill() }
}

# One k6 run of tools\k6\<Script>; returns the parsed JSON summary.
function Invoke-K6([string]$K6, [string]$Script, [hashtable]$Vars, [string]$Summary, [string[]]$Extra = @()) {
    $argList = @('run', '--quiet', '--no-color') + $Extra
    foreach ($k in $Vars.Keys) { $argList += @('-e', "$k=$($Vars[$k])") }
    $argList += @('-e', "SUMMARY=$Summary", (Join-Path $script:Repo "tools\k6\$Script"))
    $log = [System.IO.Path]::ChangeExtension($Summary, '.log')
    & $K6 @argList *> $log
    if (-not (Test-Path $Summary)) { throw "k6 produced no summary; see $log" }
    return Get-Content $Summary -Raw | ConvertFrom-Json
}

function Get-Median([double[]]$Values) {
    $s = $Values | Sort-Object
    $n = $s.Count
    if ($n -eq 0) { return [double]::NaN }
    if ($n % 2 -eq 1) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

function Get-UnixMs([string]$Iso) { return [DateTimeOffset]::Parse($Iso).ToUnixTimeMilliseconds() }

function Write-Section([string]$Title) { Write-Host ''; Write-Host "== $Title ==" -ForegroundColor Cyan }
