# Runs the parser fuzz target (plan IX "Fuzzing"). Called by: tools\build.cmd fuzz all [seconds]
# Seeds come from tests\corpus\reject (escaped format, decoded here). The generated corpus
# and any crash/timeout artifacts stay under -WorkDir.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][int]$Seconds,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$CorpusDir
)
$ErrorActionPreference = 'Stop'

function ConvertFrom-Escaped([string]$text) {
    $bytes = New-Object System.Collections.Generic.List[byte]
    $bytes.Add(3)  # first input byte selects the fuzz read size (see parser_fuzz.cpp)
    for ($i = 0; $i -lt $text.Length; $i++) {
        $c = [string]$text[$i]
        if ($c -eq "`r" -or $c -eq "`n") { continue }
        if ($c -ne '\' -or $i + 1 -ge $text.Length) { $bytes.Add([byte][char]$c); continue }
        $i++
        switch -CaseSensitive ([string]$text[$i]) {
            'r' { $bytes.Add(13) }
            'n' { $bytes.Add(10) }
            't' { $bytes.Add(9) }
            '\' { $bytes.Add(92) }
            'x' { $bytes.Add([Convert]::ToByte($text.Substring($i + 1, 2), 16)); $i += 2 }
            default { $bytes.Add(92); $bytes.Add([byte][char]$text[$i]) }
        }
    }
    return , $bytes.ToArray()
}

$seeds = Join-Path $WorkDir 'seeds'
$corpus = Join-Path $WorkDir 'corpus'
$crashes = Join-Path $WorkDir 'crashes'
foreach ($d in $seeds, $corpus, $crashes) { New-Item -ItemType Directory -Force $d | Out-Null }

Get-ChildItem $CorpusDir -Filter *.txt | ForEach-Object {
    $raw = ConvertFrom-Escaped ([System.IO.File]::ReadAllText($_.FullName))
    [System.IO.File]::WriteAllBytes((Join-Path $seeds ($_.BaseName + '.bin')), $raw)
}

Write-Host "[fuzz] $Exe for $Seconds s, seeds: $((Get-ChildItem $seeds).Count)"
& $Exe $corpus $seeds "-max_total_time=$Seconds" '-timeout=5' '-rss_limit_mb=2048' "-artifact_prefix=$crashes\" '-print_final_stats=1'
$code = $LASTEXITCODE
$found = @(Get-ChildItem $crashes -File)
if ($code -ne 0 -or $found.Count -gt 0) {
    Write-Host "[fuzz] FAILED (exit $code); artifacts in $crashes"
    exit 1
}
Write-Host '[fuzz] no crashes, hangs or sanitizer errors'
exit 0
