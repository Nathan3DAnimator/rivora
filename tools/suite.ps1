<#
  The Real Engine's test suite, quick by default (notas-dev/testes.md).

    tools/suite.ps1                       the quick kit (label "quick"): every area, ~4 min instead of ~20
    tools/suite.ps1 -Areas fisica,ia      the kit plus the whole areas you touched (labels = notas-dev/ names)
    tools/suite.ps1 -Areas ia -Somente    only those areas
    tools/suite.ps1 -Completa             the whole suite
    tools/suite.ps1 -Falhas               only what failed in the last run
    tools/suite.ps1 -Listar               the areas, their tests and which are in the kit

  Always hidden (REALENGINE_BACKGROUND=1). The tests with a window run one at a time (RESOURCE_LOCK gpu), those
  without one beside them (-Jobs). The whole log goes to <build>/Testing/suite.log; here only the summary and failures.
#>
param(
    [string[]]$Areas = @(),
    [switch]$Somente,
    [switch]$Completa,
    [switch]$Falhas,
    [switch]$Listar,
    [switch]$Parar,          # stop at the first failure
    [string]$Build = "build",
    [int]$Jobs = 8,
    [switch]$Visivel,        # windows on screen, on the -Monitor display (the user's 2nd: 18/11)
    [int]$Monitor = 2        # -Visivel: Windows' display number (REALENGINE_MONITOR; 0: the primary, maximized as before)
)

$root = Split-Path -Parent $PSScriptRoot
$buildDir = if ([System.IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $root $Build }
if (-not (Test-Path (Join-Path $buildDir "CTestTestfile.cmake"))) { Write-Host "Sem build configurada em $buildDir"; exit 2 }

$ctest = (Get-Command ctest -ErrorAction SilentlyContinue).Source
if (-not $ctest) {
    $ctest = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName -First 1
}
if (-not $ctest) { Write-Host "ctest nao encontrado"; exit 2 }

# -Areas accepts "a,b" or several values
$Areas = @($Areas | ForEach-Object { $_ -split "," } | Where-Object { $_ } | ForEach-Object { $_.Trim() })

if ($Listar) {
    # one call: every test with its labels
    $json = (& $ctest --test-dir $buildDir -C Release --show-only=json-v1 2>$null) -join "`n" | ConvertFrom-Json
    $byArea = @{}
    foreach ($t in $json.tests) {
        $labels = @(($t.properties | Where-Object { $_.name -eq "LABELS" }).value)
        $quick = $labels -contains "quick"
        foreach ($l in $labels | Where-Object { $_ -notin @("quick", "cpu", "network") }) {
            if (-not $byArea[$l]) { $byArea[$l] = @{ n = 0; kit = @() } }
            $byArea[$l].n++
            if ($quick) { $byArea[$l].kit += $t.name }
        }
    }
    foreach ($l in $byArea.Keys | Sort-Object) {
        "{0,-18} {1,3} testes  kit: {2}" -f $l, $byArea[$l].n, ($byArea[$l].kit -join " ")
    }
    "{0} testes, {1} no kit" -f @($json.tests).Count, @($json.tests | Where-Object { @(($_.properties | Where-Object { $_.name -eq "LABELS" }).value) -contains "quick" }).Count
    exit 0
}

# The installed editor using the GPU makes fps limits fail falsely (5 fps): warn, never kill the user's processes
$foreign = Get-Process realengine, Level, Packed, BpLevel -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -notlike "$root*" }
if ($foreign) { Write-Host ("Aviso: realengine aberto fora da build ({0}): a GPU dividida pode dar falhas falsas de fps." -f (($foreign | ForEach-Object { $_.Path } | Select-Object -Unique) -join ", ")) }

if (-not $Visivel) { $env:REALENGINE_BACKGROUND = "1" }
else { Remove-Item Env:REALENGINE_BACKGROUND -ErrorAction SilentlyContinue; $env:REALENGINE_MONITOR = "$Monitor" }

$args_ = @("--test-dir", $buildDir, "-C", "Release", "-j", $Jobs, "--output-on-failure")
if ($Falhas) {
    $args_ += "--rerun-failed"; $mode = "falhas da ultima"
} elseif ($Completa) {
    $mode = "suite completa"
} else {
    $set = @()
    if (-not $Somente) { $set += "quick" }
    $set += $Areas
    if (-not $set) { Write-Host "-Somente precisa de -Areas"; exit 2 }
    $args_ += @("-L", ("^(" + ($set -join "|") + ")$"))
    $mode = if ($Somente) { "areas: " + ($Areas -join ", ") } elseif ($Areas) { "kit rapido + " + ($Areas -join ", ") } else { "kit rapido" }
}
if ($Parar) { $args_ += "--stop-on-failure" }

$log = Join-Path $buildDir "Testing\suite.log"
Write-Host "Suite ($mode), log em $log ..."
$t0 = Get-Date
& $ctest @args_ -O $log 2>&1 | Out-Null # (everything is in the log; stderr would come out as a PowerShell error)
$code = $LASTEXITCODE
$secs = [int]((Get-Date) - $t0).TotalSeconds

$text = Get-Content $log -Encoding UTF8
$text | Where-Object { $_ -match "tests passed|Total Test time" } | ForEach-Object { Write-Host $_ }
$failedAt = [Array]::IndexOf($text, ($text | Where-Object { $_ -match "^The following tests FAILED" } | Select-Object -First 1))
if ($failedAt -ge 0) {
    $text[$failedAt..($text.Count - 1)] | Where-Object { $_ -match "^\s+\d+ - " } | ForEach-Object { Write-Host $_ }
    Write-Host "Linhas de falha:"
    $text | Where-Object { $_ -match "FAIL|VUID|DEVICE_LOST|Exception|Timeout" -and $_ -notmatch "^\s+\d+ - |tests FAILED" } |
        Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
}
Write-Host ("Tempo: {0}:{1:00}" -f [int][math]::Floor($secs / 60), ($secs % 60))
exit $code
