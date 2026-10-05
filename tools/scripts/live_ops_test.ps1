# Phase-2 gate (plan I, IX, X "Drain and reload safety"): reloads and drains under load
# with zero dropped requests. Real processes: lb_console, four mock backends, k6.
#   powershell -ExecutionPolicy Bypass -File tools\scripts\live_ops_test.ps1 [-Rate 2000] [-Runs 3]
#
# While k6 sends a constant arrival rate through the proxy, the script changes the running
# proxy the way an operator would, one step every StepSeconds, and waits for each step to
# take effect before the next:
#   1. add web-4                     (config file edit, picked up by the file watcher)
#   2. weights and strategy change   (file edit)
#   3. drain web-2                   (stdin "drain", like the dashboard's Drain button)
#   4. remove web-2 once drained     (file edit)
#   5. drain web-3 from the config   (file edit: "drain": "start")
#   6. a half-written config         (must be rejected and change nothing)
#   7. web-3 back in service         (file edit: "drain": "cancel")
#   8. back to round robin, "keep"   (file edit)
# PASS (every run): k6 saw zero failed requests and zero dropped iterations, every reload
# was accepted or rejected as expected, both drains completed (no timeout, no aborted
# request), and a drained backend received no request after it was drained.
param(
    [string]$K6 = '',
    [int]$Rate = 2000,
    [int]$Runs = 3,
    [int]$LatencyMs = 5,
    [int]$StepSeconds = 6,
    [string]$Preset = 'release',
    # Also record k6's per-request CSV and report when any dropped or failed iteration
    # happened relative to the steps (large: about 1 GB per minute at 5,000 req/s).
    [switch]$Timeline
)
. "$PSScriptRoot\common.ps1"
$k6 = Get-K6Path $K6
$template = Join-Path $script:Repo 'config\liveops.json'
$templateConfig = Get-Content $template -Raw | ConvertFrom-Json
$logPath = Join-Path $script:Repo $templateConfig.event_log.path
$ports = @{ 'web-1' = 9401; 'web-2' = 9402; 'web-3' = 9403; 'web-4' = 9404 }
$steps = 8
$seconds = $StepSeconds * ($steps + 2)

function Get-MockRequests([string]$Id) {
    return [long](Invoke-RestMethod -UseBasicParsing -TimeoutSec 5 "http://127.0.0.1:$($ports[$Id])/__mock/stats").requests
}

function Get-State($Metrics, [string]$Id) {
    $b = @($Metrics.backend_states | Where-Object { $_.id -eq $Id })
    if ($b.Count -eq 0) { return 'absent' }
    return $b[0].state
}

function Invoke-Run([int]$Run) {
    $out = New-RunDir "liveops-run$Run"
    $metricsDir = Join-Path $out 'metrics'
    New-Item -ItemType Directory -Force $metricsDir | Out-Null
    $cfgPath = Join-Path $out 'lb.json'
    Copy-Item $template $cfgPath
    Get-ChildItem "$logPath*" -ErrorAction SilentlyContinue | Remove-Item
    $script:poll = 0

    $mocks = @{}
    $proxy = $null
    $notes = New-Object System.Collections.Generic.List[string]
    $stepMs = New-Object System.Collections.Generic.List[long]  # when each step's change was made
    try {
        foreach ($id in $ports.Keys) { $mocks[$id] = Start-Mock $Preset $ports[$id] $id @('--latency-ms', $LatencyMs) $out }
        $proxy = Start-Proxy $Preset $cfgPath
        # lb_console prints a line per command: keep reading it, or a full pipe would block it.
        $proxyOutput = $proxy.StandardOutput.ReadToEndAsync()
        Start-Sleep -Seconds 2  # first health probes

        $metrics = { $script:poll++; Save-ProxyMetrics $proxy (Join-Path $metricsDir "m$($script:poll).json") }
        $waitFor = {
            param([scriptblock]$Condition, [string]$What)
            $deadline = (Get-Date).AddSeconds(15)
            while ((Get-Date) -lt $deadline) {
                $m = & $metrics
                if (& $Condition $m) { return $m }
                Start-Sleep -Milliseconds 100
            }
            throw "timed out waiting for: $What"
        }
        $config = Get-Content $cfgPath -Raw | ConvertFrom-Json
        $save = { $config | ConvertTo-Json -Depth 20 | Set-Content -Encoding UTF8 $cfgPath }
        $web = { param([string]$Id) $config.groups[0].backends | Where-Object { $_.id -eq $Id } }

        $summary = Join-Path $out 'k6.json'
        $csv = Join-Path $out 'k6.csv'
        $csvArgs = if ($Timeline) { @('--out', "csv=$csv") } else { @() }
        $env:K6_CSV_TIME_FORMAT = 'unix_milli'
        $k6Args = @('run', '--quiet', '--no-color') + $csvArgs + @(
                    '-e', "TARGET=http://127.0.0.1:$($config.listen.port)/", '-e', "RATE=$Rate", '-e', "DURATION=${seconds}s",
                    '-e', "SUMMARY=$summary", (Join-Path $script:Repo 'tools\k6\constant_rate.js'))
        $load = Start-Process $k6 -ArgumentList $k6Args -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'k6.log')
        $t0 = Get-Date
        $at = { param([int]$Step) while (((Get-Date) - $t0).TotalSeconds -lt $StepSeconds * $Step) { Start-Sleep -Milliseconds 20 } }

        # 1. Add web-4.
        & $at 1
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        $config.groups[0].backends += [pscustomobject][ordered]@{ id = 'web-4'; address = '127.0.0.1'; port = $ports['web-4']; weight = 1; drain = 'keep' }
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 1 } 'reload 1 (add web-4)' | Out-Null
        $notes.Add('1 added web-4')

        # 2. Weights and strategy.
        & $at 2
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        (& $web 'web-1').weight = 3
        $config.groups[0].strategy = 'least_connections'
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 2 } 'reload 2 (weights, strategy)' | Out-Null
        $notes.Add('2 web-1 weight 3, least_connections')

        # 3. Drain web-2 (operator command), wait until drained.
        & $at 3
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        $proxy.StandardInput.WriteLine('drain web-2'); $proxy.StandardInput.Flush()
        & $waitFor { param($m) (Get-State $m 'web-2') -eq 'drained' } 'web-2 drained' | Out-Null
        $web2Drained = Get-MockRequests 'web-2'
        $notes.Add("3 drained web-2 at $web2Drained requests")

        # 4. Remove web-2 from the config.
        & $at 4
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        $config.groups[0].backends = @($config.groups[0].backends | Where-Object { $_.id -ne 'web-2' })
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 3 -and (Get-State $m 'web-2') -eq 'absent' } 'reload 3 (remove web-2)' | Out-Null
        $notes.Add('4 removed web-2')

        # 5. Drain web-3 from the config.
        & $at 5
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        (& $web 'web-3').drain = 'start'
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 4 -and (Get-State $m 'web-3') -eq 'drained' } 'reload 4, web-3 drained' | Out-Null
        $web3Drained = Get-MockRequests 'web-3'
        $notes.Add("5 drained web-3 at $web3Drained requests")

        # 6. A half-written save: rejected, no effect.
        & $at 6
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        Set-Content -Encoding UTF8 $cfgPath '{ "listen": { "address": '
        & $waitFor { param($m) $m.stats.reloads_rejected -eq 1 } 'rejection of the broken config' | Out-Null
        $notes.Add('6 broken config rejected')

        # 7. web-3 back in service.
        & $at 7
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        $web3BeforeReturn = Get-MockRequests 'web-3'
        (& $web 'web-3').drain = 'cancel'
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 5 -and (Get-State $m 'web-3') -eq 'healthy' } 'reload 5, web-3 healthy' | Out-Null
        $notes.Add('7 web-3 back in service')

        # 8. Tidy up: round robin, "keep".
        & $at 8
        $stepMs.Add([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())
        (& $web 'web-3').drain = 'keep'
        $config.groups[0].strategy = 'round_robin'
        & $save
        & $waitFor { param($m) $m.stats.reloads_accepted -eq 6 } 'reload 6 (round robin)' | Out-Null
        $notes.Add('8 round robin')

        $load.WaitForExit()
        $final = & $metrics
        $counts = @{}
        foreach ($id in $ports.Keys) { $counts[$id] = Get-MockRequests $id }
        Stop-Proxy $proxy
        $proxy = $null
        $proxyOutput.Result | Set-Content (Join-Path $out 'proxy.log')
    }
    finally {
        if ($proxy) { Stop-Proxy $proxy }
        $mocks.Values | Where-Object { -not $_.HasExited } | Stop-Process -Force
    }

    $k = Get-Content $summary -Raw | ConvertFrom-Json
    $events = Get-Content $logPath | ForEach-Object { $_ | ConvertFrom-Json }
    Copy-Item $logPath (Join-Path $out 'events.jsonl')
    $drains = @($events | Where-Object { $_.event -eq 'drain_completed' })
    $drainStarts = @($events | Where-Object { $_.event -eq 'drain_started' })
    $failed = [long][math]::Round($k.failed_rate * $k.requests)
    $s = $final.stats
    $checks = [ordered]@{
        no_failed_requests = $failed -eq 0
        no_dropped_iterations = $k.dropped_iterations -eq 0
        no_proxy_errors = $s.error_responses -eq 0
        reloads_6_accepted_1_rejected = $s.reloads_accepted -eq 6 -and $s.reloads_rejected -eq 1
        drains_completed_2_none_timed_out = $s.drains_completed -eq 2 -and $s.drains_timed_out -eq 0 -and $s.drain_aborted_requests -eq 0
        web2_nothing_after_drained = $counts['web-2'] -eq $web2Drained
        web3_nothing_while_drained = $web3BeforeReturn -eq $web3Drained
        web3_traffic_after_return = $counts['web-3'] -gt $web3BeforeReturn
        web4_took_traffic = $counts['web-4'] -gt 0
    }
    $pass = -not ($checks.Values -contains $false)

    # Optional: when did k6 drop or fail iterations, relative to the nearest step before them?
    $dropEvents = @()  # not $timeline: PowerShell names are case-insensitive, that is the -Timeline switch
    if ($Timeline -and (Test-Path $csv)) {
        $reader = [System.IO.StreamReader]::new($csv)
        $header = $reader.ReadLine().Split(',')
        $iName = [array]::IndexOf($header, 'metric_name'); $iTime = [array]::IndexOf($header, 'timestamp'); $iValue = [array]::IndexOf($header, 'metric_value')
        while ($null -ne ($line = $reader.ReadLine())) {
            $f = $line.Split(',')
            $name = $f[$iName]
            if ($name -ne 'dropped_iterations' -and $name -ne 'http_req_failed') { continue }
            $value = [double]::Parse($f[$iValue], [Globalization.CultureInfo]::InvariantCulture)
            if ($value -lt 1) { continue }
            $t = [long]$f[$iTime]
            $before = @($stepMs | Where-Object { $_ -le $t })
            $step = $before.Count
            $since = if ($step -gt 0) { $t - $before[-1] } else { $null }
            $dropEvents += [ordered]@{ metric = $name; count = $value; unix_ms = $t; after_step = $step; ms_after_step = $since }
        }
        $reader.Close()
        Remove-Item $csv  # keep the summary, not the gigabyte
    }
    $result = [ordered]@{
        run = $Run; rate = $Rate; seconds = $seconds; pass = $pass
        requests = $k.requests; achieved_rps = [math]::Round($k.achieved_rps, 1)
        failed = $failed; dropped_iterations = $k.dropped_iterations
        k6_p50_ms = $k.p50_ms; k6_p99_ms = $k.p99_ms; k6_max_ms = $k.max_ms
        reloads_accepted = $s.reloads_accepted; reloads_rejected = $s.reloads_rejected
        drains_completed = $s.drains_completed; drain_durations_ms = @($drains | ForEach-Object { $_.duration_ms })
        drain_in_flight_at_start = @($drainStarts | ForEach-Object { $_.in_flight })
        backend_requests = $counts
        checks = $checks
        steps = $notes
        step_unix_ms = $stepMs
        drops_and_failures = $dropEvents
        out = $out
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'results.json') -Encoding UTF8
    return $result
}

$results = @()
for ($r = 1; $r -le $Runs; ++$r) {
    Write-Section "Run $r of $Runs ($Rate req/s, $seconds s)"
    $res = Invoke-Run $r
    Write-Host ("  {0}: {1} requests, {2} failed, {3} dropped, p99 {4:N2} ms, drains {5} ms with {6} in flight" -f `
        $(if ($res.pass) { 'PASS' } else { 'FAIL' }), $res.requests, $res.failed, $res.dropped_iterations, $res.k6_p99_ms,
        ($res.drain_durations_ms -join '/'), ($res.drain_in_flight_at_start -join '/'))
    if (-not $res.pass) { $res.checks | ConvertTo-Json | Write-Host }
    $results += $res
}

$allPass = -not (@($results | ForEach-Object { $_.pass }) -contains $false)
$summaryOut = [ordered]@{
    pass = $allPass
    runs = $Runs
    rate = $Rate
    median_requests = Get-Median ($results | ForEach-Object { [double]$_.requests })
    median_achieved_rps = Get-Median ($results | ForEach-Object { [double]$_.achieved_rps })
    median_k6_p99_ms = Get-Median ($results | ForEach-Object { [double]$_.k6_p99_ms })
    failed_total = [long](($results | ForEach-Object { $_.failed }) | Measure-Object -Sum).Sum
    dropped_total = [long](($results | ForEach-Object { $_.dropped_iterations }) | Measure-Object -Sum).Sum
    run_dirs = @($results | ForEach-Object { $_.out })
}
$summaryDir = New-RunDir 'liveops'
$summaryOut | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $summaryDir 'results.json') -Encoding UTF8
Write-Section "Live operations gate: $(if ($allPass) { 'PASS' } else { 'FAIL' })  ($summaryDir)"
$summaryOut | ConvertTo-Json -Depth 4
if (-not $allPass) { exit 1 }
