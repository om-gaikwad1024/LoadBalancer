# Phase-gate backend-kill test (plan I, IX "Chaos", IV.10/IV.16). Real processes, real kill.
#   powershell -ExecutionPolicy Bypass -File tools\scripts\kill_test.ps1 [-Rate 500]
#
# Constant-arrival-rate load through the proxy to 3 backends; at KillAt seconds one backend
# process is terminated (hard kill), at RestartAt it is started again on the same port.
# Timestamps: kill/restart from this script, exclusion/re-inclusion from the event log,
# per-request failures from k6's CSV output - all wall clock on the same machine.
# PASS: zero failed requests after exclusion (plus SlackMs for requests already completing),
#       exclusion within the detection window, re-inclusion after restart, zero errors before the kill.
param(
    [string]$K6 = '',
    [int]$Rate = 500,
    [int]$Seconds = 40,
    [int]$KillAt = 10,
    [int]$RestartAt = 25,
    [double]$SlackMs = 100,
    [string]$Preset = 'release'
)
. "$PSScriptRoot\common.ps1"
$k6 = Get-K6Path $K6
$out = New-RunDir 'kill'
$configPath = Join-Path $script:Repo 'config\killtest.json'
$config = Get-Content $configPath -Raw | ConvertFrom-Json
$health = $config.groups[0].health
$windowMs = $health.interval_ms * $health.unhealthy_threshold + $health.timeout_ms
$logPath = Join-Path $script:Repo $config.event_log.path
Get-ChildItem "$logPath*" -ErrorAction SilentlyContinue | Remove-Item

$mocks = @{}
$proxy = $null
$victim = 'web-2'
$victimPort = 9202
try {
    foreach ($i in 1..3) { $mocks["web-$i"] = Start-Mock $Preset (9200 + $i) "web-$i" @() $out }
    $proxy = Start-Proxy $Preset $configPath
    Start-Sleep -Seconds 2  # let the first health probes run

    $csv = Join-Path $out 'k6.csv'
    $summary = Join-Path $out 'k6.json'
    $env:K6_CSV_TIME_FORMAT = 'unix_milli'
    $k6Args = @('run', '--quiet', '--no-color', '--out', "csv=$csv",
                '-e', 'TARGET=http://127.0.0.1:8091/', '-e', "RATE=$Rate", '-e', "DURATION=${Seconds}s",
                '-e', "SUMMARY=$summary", (Join-Path $script:Repo 'tools\k6\constant_rate.js'))
    $load = Start-Process $k6 -ArgumentList $k6Args -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'k6.log')
    $t0 = Get-Date

    while (((Get-Date) - $t0).TotalSeconds -lt $KillAt) { Start-Sleep -Milliseconds 20 }
    $killMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    Stop-Process $mocks[$victim] -Force   # TerminateProcess: no graceful shutdown
    Write-Host "  killed $victim at +$KillAt s"

    while (((Get-Date) - $t0).TotalSeconds -lt $RestartAt) { Start-Sleep -Milliseconds 20 }
    $restartMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $mocks[$victim] = Start-Mock $Preset $victimPort $victim @() $out
    Write-Host "  restarted $victim at +$RestartAt s"

    $load.WaitForExit()
    $final = Save-ProxyMetrics $proxy (Join-Path $out 'proxy-final.json')
    Stop-Proxy $proxy; $proxy = $null
}
finally {
    if ($proxy) { Stop-Proxy $proxy }
    $mocks.Values | Where-Object { -not $_.HasExited } | Stop-Process -Force
}

# ---- Event log: exclusion and re-inclusion ---------------------------------------------
$events = Get-Content $logPath | ForEach-Object { $_ | ConvertFrom-Json }
Copy-Item $logPath (Join-Path $out 'events.jsonl')
$down = $events | Where-Object { $_.event -eq 'backend_marked_unhealthy' -and $_.backend -eq $victim } | Select-Object -First 1
$up = $events | Where-Object { $_.event -eq 'backend_marked_healthy' -and $_.backend -eq $victim } | Select-Object -First 1
$excludedMs = if ($down) { Get-UnixMs $down.ts } else { $null }
$reincludedMs = if ($up) { Get-UnixMs $up.ts } else { $null }

# ---- k6 CSV: every failed request and when it completed --------------------------------
$reader = [System.IO.StreamReader]::new($csv)
$header = $reader.ReadLine().Split(',')
$iName = [array]::IndexOf($header, 'metric_name'); $iTime = [array]::IndexOf($header, 'timestamp'); $iValue = [array]::IndexOf($header, 'metric_value')
$total = 0; $failures = New-Object System.Collections.Generic.List[long]
while ($null -ne ($line = $reader.ReadLine())) {
    $f = $line.Split(',')
    if ($f[$iName] -ne 'http_req_failed') { continue }
    $total++
    # k6 writes values as 0.000000 / 1.000000: compare numerically.
    if ([double]::Parse($f[$iValue], [Globalization.CultureInfo]::InvariantCulture) -ge 1) { $failures.Add([long]$f[$iTime]) }
}
$reader.Close()

$cut = if ($excludedMs) { $excludedMs + $SlackMs } else { [long]::MaxValue }
$before = @($failures | Where-Object { $_ -lt $killMs }).Count
$during = @($failures | Where-Object { $_ -ge $killMs -and $_ -le $cut }).Count
$after = @($failures | Where-Object { $_ -gt $cut }).Count
$failoverMs = if ($excludedMs) { $excludedMs - $killMs } else { $null }
$reinclusionMs = if ($reincludedMs) { $reincludedMs - $restartMs } else { $null }

$pass = ($excludedMs -ne $null) -and ($failoverMs -le $windowMs + $SlackMs) -and ($after -eq 0) -and ($before -eq 0) -and ($reincludedMs -ne $null)
$results = [ordered]@{
    rate = $Rate; seconds = $Seconds; requests = $total
    detection_window_ms = $windowMs
    failover_ms = $failoverMs
    failed_before_kill = $before
    failed_between_kill_and_exclusion = $during
    failed_after_exclusion = $after
    reinclusion_after_restart_ms = $reinclusionMs
    exclusion_reason = if ($down) { $down.reason } else { $null }
    stale_retries = $final.stats.stale_retries
    pass = $pass
    event_log = @($events | ForEach-Object { "$($_.ts) $($_.event) $($_.message)" })
}
$results | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'results.json') -Encoding UTF8
Write-Section "Kill test: $(if ($pass) { 'PASS' } else { 'FAIL' })  ($out)"
$results | ConvertTo-Json -Depth 5
if (-not $pass) { exit 1 }
