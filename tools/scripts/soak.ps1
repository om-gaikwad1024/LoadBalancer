# Phase-gate soak test (plan IX "Soak", X "Stability"): no handle or memory growth under long
# continuous load.
#   powershell -ExecutionPolicy Bypass -File tools\scripts\soak.ps1 [-Minutes 30]
#
# Load: keep-alive traffic plus a stream of Connection: close requests (socket churn), both
# constant-arrival-rate. Every FlapSeconds one backend's health endpoint fails for a few
# seconds, so it is marked down and up again (pool close_idle, event log, re-inclusion).
# The proxy process's handle count, private bytes and thread count are sampled every
# SampleSeconds (the Performance Monitor counters of plan IX, read through Get-Process).
# PASS: after warm-up, end-of-run handles, private bytes and threads stay at the baseline
# (within HandleSlack, PrivateSlackMB) and k6 saw no failed requests.
param(
    [string]$K6 = '',
    [int]$Minutes = 30,
    [int]$Rate = 1000,
    [int]$ChurnRate = 50,
    [int]$SampleSeconds = 10,
    [int]$WarmupSeconds = 120,
    [int]$FlapSeconds = 120,
    [int]$HandleSlack = 30,
    [double]$PrivateSlackMB = 8,
    [string]$Preset = 'release'
)
. "$PSScriptRoot\common.ps1"
$k6 = Get-K6Path $K6
$out = New-RunDir 'soak'
$configPath = Join-Path $script:Repo 'config\soak.json'
$config = Get-Content $configPath -Raw | ConvertFrom-Json
Get-ChildItem (Join-Path $script:Repo "$($config.event_log.path)*") -ErrorAction SilentlyContinue | Remove-Item

$mocks = @()
$proxy = $null
$samples = New-Object System.Collections.Generic.List[object]
try {
    foreach ($i in 1..3) { $mocks += Start-Mock $Preset (9300 + $i) "web-$i" @() $out }
    $proxy = Start-Proxy $Preset $configPath

    $summary = Join-Path $out 'k6.json'
    $k6Args = @('run', '--quiet', '--no-color', '-e', 'TARGET=http://127.0.0.1:8092/', '-e', "RATE=$Rate",
                '-e', "CHURN_RATE=$ChurnRate", '-e', "DURATION=${Minutes}m", '-e', "SUMMARY=$summary",
                (Join-Path $script:Repo 'tools\k6\soak.js'))
    $load = Start-Process $k6 -ArgumentList $k6Args -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $out 'k6.log')
    $t0 = Get-Date
    $nextFlap = $FlapSeconds
    $flaps = 0
    while (-not $load.HasExited) {
        $elapsed = ((Get-Date) - $t0).TotalSeconds
        $p = Get-Process -Id $proxy.Id
        $samples.Add([pscustomobject]@{
            elapsed_s = [math]::Round($elapsed); handles = $p.HandleCount
            private_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 2); working_set_mb = [math]::Round($p.WorkingSet64 / 1MB, 2)
            threads = $p.Threads.Count
        })
        if ($elapsed -ge $nextFlap) {  # web-3's health endpoint fails for a while: marked down, then up
            Invoke-WebRequest -UseBasicParsing 'http://127.0.0.1:9303/__mock/set?health_status=503' | Out-Null
            Start-Sleep -Seconds 3
            Invoke-WebRequest -UseBasicParsing 'http://127.0.0.1:9303/__mock/set?health_status=200' | Out-Null
            $nextFlap += $FlapSeconds
            $flaps++
        }
        if ([int]$elapsed % 60 -lt $SampleSeconds) {
            $last = $samples[$samples.Count - 1]
            Write-Host ("  +{0,5}s handles {1,5} private {2,7} MB threads {3}" -f $last.elapsed_s, $last.handles, $last.private_mb, $last.threads)
        }
        Start-Sleep -Seconds $SampleSeconds
    }
    $final = Save-ProxyMetrics $proxy (Join-Path $out 'proxy-final.json')
    Stop-Proxy $proxy; $proxy = $null
}
finally {
    if ($proxy) { Stop-Proxy $proxy }
    $mocks | Where-Object { -not $_.HasExited } | Stop-Process -Force
}
$samples | Export-Csv (Join-Path $out 'samples.csv') -NoTypeInformation

# Baseline: median of the samples between warm-up and warm-up + 60 s; end: median of the
# last 3 samples. Medians smooth out connections that happen to be open at sample time.
$warm = $samples | Where-Object { $_.elapsed_s -ge $WarmupSeconds -and $_.elapsed_s -lt $WarmupSeconds + 60 }
$tail = $samples | Select-Object -Last 3
$base = [ordered]@{ handles = Get-Median ($warm | ForEach-Object handles); private_mb = Get-Median ($warm | ForEach-Object private_mb); threads = Get-Median ($warm | ForEach-Object threads) }
$end = [ordered]@{ handles = Get-Median ($tail | ForEach-Object handles); private_mb = Get-Median ($tail | ForEach-Object private_mb); threads = Get-Median ($tail | ForEach-Object threads) }

# Trend after warm-up (least squares), per hour.
$after = @($samples | Where-Object { $_.elapsed_s -ge $WarmupSeconds })
function Get-Slope($xs, $ys) {
    $n = $xs.Count; $mx = ($xs | Measure-Object -Average).Average; $my = ($ys | Measure-Object -Average).Average
    $num = 0.0; $den = 0.0
    for ($i = 0; $i -lt $n; $i++) { $num += ($xs[$i] - $mx) * ($ys[$i] - $my); $den += ($xs[$i] - $mx) * ($xs[$i] - $mx) }
    if ($den -eq 0) { return 0 } else { return $num / $den }
}
$xs = $after | ForEach-Object { [double]$_.elapsed_s }
$k6Summary = Get-Content $summary -Raw | ConvertFrom-Json
$growth = [ordered]@{
    handles = $end.handles - $base.handles
    private_mb = [math]::Round($end.private_mb - $base.private_mb, 2)
    threads = $end.threads - $base.threads
    handles_per_hour_trend = [math]::Round((Get-Slope $xs ($after | ForEach-Object { [double]$_.handles })) * 3600, 1)
    private_mb_per_hour_trend = [math]::Round((Get-Slope $xs ($after | ForEach-Object { [double]$_.private_mb })) * 3600, 2)
}
$pass = ($growth.handles -le $HandleSlack) -and ($growth.private_mb -le $PrivateSlackMB) -and ($growth.threads -le 0) -and ($k6Summary.failed_rate -eq 0)
$results = [ordered]@{
    minutes = $Minutes; rate = $Rate; churn_rate = $ChurnRate; health_flaps = $flaps
    k6 = $k6Summary; baseline = $base; end = $end; growth = $growth
    proxy = [ordered]@{ requests = $final.stats.requests_completed; connections = $final.stats.connections_accepted
                        backend_connections_opened = $final.stats.backend_connections_opened
                        marked_down = $final.stats.backends_marked_down; marked_up = $final.stats.backends_marked_up
                        events_dropped = $final.stats.events_dropped }
    pass = $pass
}
$results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $out 'results.json') -Encoding UTF8
Write-Section "Soak: $(if ($pass) { 'PASS' } else { 'FAIL' })  ($out)"
$results | ConvertTo-Json -Depth 6
if (-not $pass) { exit 1 }
