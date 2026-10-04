# Phase-gate benchmark (plan X). Real measurements only; results go to build\gate\bench-*\.
#   powershell -ExecutionPolicy Bypass -File tools\scripts\bench.ps1 [-K6 <k6.exe>] [-P99TargetMs 10]
#
# 1. Mock backend's own limit, measured directly (it serves as the direct-to-backend baseline).
# 2. Throughput: highest constant-arrival rate the proxy sustains with p99 <= target, no errors
#    and < 0.1% dropped iterations; coarse search (one 15 s run per rate), then 3 x 30 s runs of
#    the best rate, median reported.
# 3. Proxy overhead and tail latency: backends with a fixed 5 ms delay (k6 cannot resolve
#    sub-millisecond loopback latencies on Windows), 3 runs direct to one backend and 3 runs
#    through the proxy at the same rate; proxy's own percentiles compared with k6's.
# 4. Connection reuse: requests per new backend connection.
param(
    [string]$K6 = '',
    [double]$P99TargetMs = 10,
    [int[]]$Rates = @(1000, 2000, 2500, 3000, 3500, 4000, 5000, 6000),
    [int]$OverheadRate = 1000,
    [int]$Runs = 3,
    [string]$Preset = 'release'
)
. "$PSScriptRoot\common.ps1"
$k6 = Get-K6Path $K6
$out = New-RunDir 'bench'
$config = Join-Path $script:Repo 'config\bench.json'
$results = [ordered]@{ p99_target_ms = $P99TargetMs; runs_per_measurement = $Runs }

function Test-Pass($r, [int]$Rate) {
    return ($r.failed_rate -eq 0) -and ($r.dropped_iterations -le [math]::Ceiling($r.rate_target * 0.001 * 15)) -and
           ($r.p99_ms -le $P99TargetMs) -and ($r.achieved_rps -ge $Rate * 0.98)
}

function Search-MaxRate([string]$Label, [string]$Target) {
    $best = 0
    $steps = @()
    foreach ($rate in $Rates) {
        $vus = [math]::Max(50, [int]($rate / 20))
        $r = Invoke-K6 $k6 'constant_rate.js' @{ TARGET = $Target; RATE = $rate; DURATION = '15s'; VUS = $vus } (Join-Path $out "$Label-search-$rate.json")
        $ok = Test-Pass $r $rate
        $steps += [ordered]@{ rate = $rate; achieved = [math]::Round($r.achieved_rps, 1); p99_ms = $r.p99_ms; max_ms = $r.max_ms; failed_rate = $r.failed_rate; dropped = $r.dropped_iterations; pass = $ok }
        Write-Host ("  {0,-6} {1,6}/s -> achieved {2,8:N1}/s p99 {3,7:N2} ms max {4,8:N2} ms failed {5:P2} dropped {6} {7}" -f $Label, $rate, $r.achieved_rps, $r.p99_ms, $r.max_ms, $r.failed_rate, $r.dropped_iterations, $(if ($ok) { 'PASS' } else { 'FAIL' }))
        if (-not $ok) { break }
        $best = $rate
    }
    return @{ best = $best; steps = $steps }
}

function Measure-Runs([string]$Label, [string]$Target, [int]$Rate, $Proxy) {
    $vus = [math]::Max(50, [int]($Rate / 20))
    $runList = @()
    # Warm-up: pools, caches, k6 VUs.
    Invoke-K6 $k6 'constant_rate.js' @{ TARGET = $Target; RATE = $Rate; DURATION = '5s'; VUS = $vus } (Join-Path $out "$Label-warmup.json") | Out-Null
    for ($i = 1; $i -le $Runs; $i++) {
        $r = Invoke-K6 $k6 'constant_rate.js' @{ TARGET = $Target; RATE = $Rate; DURATION = '30s'; VUS = $vus } (Join-Path $out "$Label-run$i.json")
        $run = [ordered]@{ k6 = $r }
        if ($Proxy) {
            # bench.json has a 30 s live window: it covers exactly this run.
            $run.proxy = Save-ProxyMetrics $Proxy (Join-Path $out "$Label-run$i-proxy.json")
        }
        $runList += $run
        Write-Host ("  {0} run {1}: {2:N1}/s k6 p50 {3:N2} p99 {4:N2} max {5:N2} ms" -f $Label, $i, $r.achieved_rps, $r.p50_ms, $r.p99_ms, $r.max_ms)
    }
    return $runList
}

function Summarize($runList) {
    $k = $runList | ForEach-Object { $_.k6 }
    $s = [ordered]@{
        achieved_rps = Get-Median ($k | ForEach-Object { $_.achieved_rps })
        k6_p50_ms = Get-Median ($k | ForEach-Object { $_.p50_ms })
        k6_p95_ms = Get-Median ($k | ForEach-Object { $_.p95_ms })
        k6_p99_ms = Get-Median ($k | ForEach-Object { $_.p99_ms })
        k6_max_ms = Get-Median ($k | ForEach-Object { $_.max_ms })
        failed_rate_max = ($k | Measure-Object failed_rate -Maximum).Maximum
    }
    if ($runList[0].proxy) {
        $p = $runList | ForEach-Object { $_.proxy.system }
        $s.proxy_p50_ms = Get-Median ($p | ForEach-Object { $_.total_window.p50_ms })
        $s.proxy_p95_ms = Get-Median ($p | ForEach-Object { $_.total_window.p95_ms })
        $s.proxy_p99_ms = Get-Median ($p | ForEach-Object { $_.total_window.p99_ms })
        $s.proxy_max_ms = Get-Median ($p | ForEach-Object { $_.total_window.max_ms })
        $s.proxy_backend_p50_ms = Get-Median ($p | ForEach-Object { $_.backend_window.p50_ms })
        $s.proxy_backend_p99_ms = Get-Median ($p | ForEach-Object { $_.backend_window.p99_ms })
    }
    return $s
}

$mocks = @()
$proxy = $null
try {
    # ---- 1. Mock backend alone -------------------------------------------------------
    Write-Section 'Mock backend limit (direct, no proxy)'
    $mocks += Start-Mock $Preset 9101 'web-1' @() $out
    $mockSearch = Search-MaxRate 'mock' 'http://127.0.0.1:9101/'
    $results.mock_limit = $mockSearch
    Stop-Process $mocks[0] -Force; $mocks = @()

    # ---- 2. Proxy throughput, 3 backends ------------------------------------------------
    Write-Section 'Proxy throughput (3 backends, round robin)'
    foreach ($i in 1..3) { $mocks += Start-Mock $Preset (9100 + $i) "web-$i" @() $out }
    $proxy = Start-Proxy $Preset $config
    $search = Search-MaxRate 'proxy' 'http://127.0.0.1:8090/'
    $results.proxy_search = $search
    $rate = $search.best
    $confirmed = $null
    while ($rate -gt 0 -and -not $confirmed) {
        Write-Host "  confirming $rate/s with $Runs x 30 s"
        $confirmRuns = Measure-Runs "throughput-$rate" 'http://127.0.0.1:8090/' $rate $proxy
        $summary = Summarize $confirmRuns
        if ($summary.k6_p99_ms -le $P99TargetMs -and $summary.failed_rate_max -eq 0) { $confirmed = $summary; $confirmed.rate = $rate }
        else { $idx = [array]::IndexOf($Rates, $rate); $rate = if ($idx -gt 0) { $Rates[$idx - 1] } else { 0 } }
    }
    $results.throughput = $confirmed
    $stats = Save-ProxyMetrics $proxy (Join-Path $out 'proxy-final.json')
    $results.connection_reuse = [ordered]@{
        requests = $stats.stats.requests_completed
        backend_connections_opened = $stats.stats.backend_connections_opened
        requests_per_new_connection = [math]::Round($stats.stats.requests_completed / [math]::Max(1, $stats.stats.backend_connections_opened), 1)
    }
    Stop-Proxy $proxy; $proxy = $null
    $mocks | Stop-Process -Force; $mocks = @()

    # ---- 3. Overhead and tail latency with a 5 ms backend ------------------------------
    Write-Section "Proxy overhead at $OverheadRate/s (backends delay 5 ms)"
    foreach ($i in 1..3) { $mocks += Start-Mock $Preset (9100 + $i) "web-$i" @('--latency-ms', '5') $out }
    $direct = Summarize (Measure-Runs 'direct-5ms' 'http://127.0.0.1:9101/' $OverheadRate $null)
    $proxy = Start-Proxy $Preset $config
    $via = Summarize (Measure-Runs 'proxy-5ms' 'http://127.0.0.1:8090/' $OverheadRate $proxy)
    $results.overhead = [ordered]@{
        rate = $OverheadRate
        direct = $direct
        via_proxy = $via
        overhead_p50_ms = [math]::Round($via.k6_p50_ms - $direct.k6_p50_ms, 3)
        overhead_p99_ms = [math]::Round($via.k6_p99_ms - $direct.k6_p99_ms, 3)
        proxy_internal_overhead_p50_ms = [math]::Round($via.proxy_p50_ms - $via.proxy_backend_p50_ms, 3)
    }
    Stop-Proxy $proxy; $proxy = $null
}
finally {
    if ($proxy) { Stop-Proxy $proxy }
    $mocks | Where-Object { -not $_.HasExited } | Stop-Process -Force
}

$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$os = Get-CimInstance Win32_OperatingSystem
$results.environment = [ordered]@{
    cpu = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; logical = $cpu.NumberOfLogicalProcessors
    ram_gb = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
    os = "$($os.Caption) $($os.Version)"; k6 = (& $k6 version).Trim(); date = (Get-Date).ToString('yyyy-MM-dd HH:mm')
    topology = 'k6, proxy and 3 mock backends on the same machine'
}
$results | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $out 'results.json') -Encoding UTF8
Write-Section "Results: $out\results.json"
$results | ConvertTo-Json -Depth 10
