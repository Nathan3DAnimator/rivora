<#
  The Rivora Engine's suite (item 22, 2026-11-25; notas-dev/testes.md): the one run before publishing.

    tools/suite.ps1 -Visivel                  the whole suite, windows on the 2nd monitor (the user's way)
    tools/suite.ps1                           the same, hidden (REALENGINE_BACKGROUND=1)
    tools/suite.ps1 -Area render,fisica       only those areas
    tools/suite.ps1 -Falhas                   only what failed in the last run
    tools/suite.ps1 -Listar                   the areas and their tests
    tools/suite.ps1 -GravarReferencias        the render_ref pictures written again (then looked at, by eye)
    tools/suite.ps1 -Forcar                   runs even with the GPU shared (the check below only warns)
    tools/suite.ps1 -Resumo                   the last run's summary again, from its logs

  First the machine: what shares the GPU (OVRServer, ffmpeg, an editor outside the build) makes the fps limits fail
  falsely: it stops and says what is open. Then the tests without a window side by side, then those with one, one at
  a time. At the end one screen: each area PASSOU / FALHOU, the failures' FAIL lines, the time and the log.
  The other registered tests (label "regressao": each fixed bug's own) run by name: ctest -R <name>.
#>
param(
    [string[]]$Area = @(),
    [switch]$Falhas,
    [switch]$Listar,
    [switch]$Forcar,
    [switch]$GravarReferencias,
    [switch]$Resumo,         # only the summary again, from the last run's logs (nothing run)
    [string]$Build = "build",
    [int]$Jobs = 8,
    [switch]$Visivel,        # windows on screen, on the -Monitor display (the user's 2nd: 18/11)
    [int]$Monitor = 2        # -Visivel: Windows' display number (REALENGINE_MONITOR; 0: the primary)
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
$Area = @($Area | ForEach-Object { $_ -split "," } | Where-Object { $_ } | ForEach-Object { $_.Trim() })

# The suite's tests, their area and whether they open a window: one call (the labels)
$json = (& $ctest --test-dir $buildDir -C Release --show-only=json-v1 2>$null) -join "`n" | ConvertFrom-Json
$tests = @()
foreach ($t in $json.tests) {
    $labels = @(($t.properties | Where-Object { $_.name -eq "LABELS" }).value)
    if ($labels -notcontains "suite") { continue }
    $a = ($labels | Where-Object { $_ -like "s_*" } | Select-Object -First 1) -replace "^s_", ""
    $tests += [pscustomobject]@{ name = $t.name; area = $a; cpu = ($labels -contains "cpu") }
}
$order = @("inicio-e-projetos", "editor", "importar", "render", "materiais", "animacao-characters", "fisica", "grafos-scripts",
           "ia-navegacao", "audio", "ui-do-jogo-localizacao", "sistemas-de-jogo", "niveis-streaming", "multiplayer", "empacotar",
           "vr-ar", "atualizador-assinatura", "mcp", "plugins", "assistente", "troca-de-projeto", "memoria-travadas")
$areas = @($order | Where-Object { $a = $_; $tests | Where-Object { $_.area -eq $a } }) +
         @($tests.area | Sort-Object -Unique | Where-Object { $_ -notin $order })

if ($Listar) {
    foreach ($a in $areas) {
        $ts = @($tests | Where-Object { $_.area -eq $a })
        "{0,-24} {1,2}  {2}" -f $a, $ts.Count, (($ts | ForEach-Object { $_.name }) -join " ")
    }
    "{0} testes em {1} areas" -f $tests.Count, $areas.Count
    exit 0
}

$failedFile = Join-Path $buildDir "Testing\suite_failed.txt"
if ($Area) {
    $bad = @($Area | Where-Object { $_ -notin $areas })
    if ($bad) { Write-Host ("Area desconhecida: {0} (tools/suite.ps1 -Listar)" -f ($bad -join ", ")); exit 2 }
    $tests = @($tests | Where-Object { $_.area -in $Area })
}
if ($Falhas) {
    if (-not (Test-Path $failedFile)) { Write-Host "Nenhuma falha registrada da ultima execucao."; exit 0 }
    $last = @(Get-Content $failedFile | Where-Object { $_ })
    $tests = @($tests | Where-Object { $_.name -in $last })
}
if (-not $tests) { Write-Host "Nenhum teste a rodar."; exit 0 }

# (2026-11-25) The build's own processes left alive (a test's game or editor that outlived it: a VR Preview's game held
# the GPU through two suites): ended, and said, before and after the run. Never the user's (outside the build)
function Stop-Leftovers($when) {
    $left = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like "$buildDir*" -and $_.ProcessName -like "rivora*" })
    foreach ($p in $left) {
        Write-Host ("Aviso ({0}): {1} (pid {2}) do build ficou aberto desde {3:HH:mm}; fechado." -f $when, $p.ProcessName, $p.Id, $p.StartTime) -ForegroundColor Yellow
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    return $left.Count
}
$leftBefore = if ($Resumo) { 0 } else { Stop-Leftovers "antes" }

# The machine: what shares the GPU, never killed (the user's)
$noisy = @(Get-Process OVRServer_x64, ffmpeg -ErrorAction SilentlyContinue) + # (OVRServiceLauncher, the service alone, takes no GPU)
         @(Get-Process rivora, realengine, Level, Packed, BpLevel -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -notlike "$root*" })
if ($noisy -and -not $Resumo) {
    Write-Host "A GPU esta dividida com:" -ForegroundColor Yellow
    $noisy | ForEach-Object { Write-Host ("  {0} ({1})" -f $_.ProcessName, $(if ($_.Path) { $_.Path } else { "pid $($_.Id)" })) }
    if (-not $Forcar) { Write-Host "Feche-os (os limites de fps falhariam por eles) ou rode com -Forcar."; exit 3 }
    Write-Host "-Forcar: seguindo; uma falha de fps pode ser deles." -ForegroundColor Yellow
}

if (-not $Visivel) { $env:REALENGINE_BACKGROUND = "1" }
else { Remove-Item Env:REALENGINE_BACKGROUND -ErrorAction SilentlyContinue; $env:REALENGINE_MONITOR = "$Monitor" }
if ($GravarReferencias) { $env:REALENGINE_UPDATE_REFS = "1" } else { Remove-Item Env:REALENGINE_UPDATE_REFS -ErrorAction SilentlyContinue }

$log = Join-Path $buildDir "Testing\suite.log"
$logCpu = Join-Path $buildDir "Testing\suite_cpu.log"
$logGpu = Join-Path $buildDir "Testing\suite_gpu.log"
function Get-Regex($list) { "^(" + (($list | ForEach-Object { [regex]::Escape($_.name) }) -join "|") + ")$" }
$cpuT = @($tests | Where-Object { $_.cpu }); $gpuT = @($tests | Where-Object { -not $_.cpu })
$secs = 0; $leftAfter = 0
if (-not $Resumo) {
    Remove-Item $log, $logCpu, $logGpu -ErrorAction SilentlyContinue
    Write-Host ("Suite: {0} testes em {1} areas ({2} sem janela em paralelo, {3} com janela um por vez). Log: {4}" -f $tests.Count,
                @($tests.area | Sort-Object -Unique).Count, $cpuT.Count, $gpuT.Count, $log)
    $t0 = Get-Date
    if ($cpuT) { & $ctest --test-dir $buildDir -C Release -j $Jobs --output-on-failure -R (Get-Regex $cpuT) -O $logCpu 2>&1 | Out-Null }
    if ($gpuT) { & $ctest --test-dir $buildDir -C Release -j 1 --output-on-failure -R (Get-Regex $gpuT) -O $logGpu 2>&1 | Out-Null }
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    $leftAfter = Stop-Leftovers "depois"
} else { Write-Host "-Resumo: o resumo dos logs da ultima execucao ($logCpu, $logGpu)" }
$text = @()
foreach ($l in @($logCpu, $logGpu)) { if (Test-Path $l) { $text += Get-Content $l -Encoding UTF8 } }
$text | Set-Content $log -Encoding UTF8

# Each test's result (the fixtures' own setup tests count too: a failed project clean fails its area's tests anyway)
$res = @{}
foreach ($l in $text) {
    if ($l -match "Test\s+#\d+:\s+(\S+)\s+\.*\s*(\*\*\*)?(Passed|Failed|Not Run|Timeout|Exception|Skipped|\*\*\*Skipped)?.*?([\d.]+) sec") {
        $tname = $Matches[1]; $tsecs = [double]$Matches[4] # (kept before the next -match overwrites $Matches)
        $st = if ($l -match "Passed") { "ok" } elseif ($l -match "Skipped|Not Run.*Disabled") { "skip" } else { "fail" }
        $res[$tname] = [pscustomobject]@{ st = $st; secs = $tsecs }
    }
}
# The first FAIL line of each failed test's output
function Get-FailLine($name) {
    $in = $false; $first = ""
    foreach ($l in $text) {
        if ($in -and -not $first -and $l.Trim() -and $l -notmatch "Test\s+#|^\s+Start\s") { $first = $l.Trim() } # (no FAIL line: its first one)
        if ($l -match "Test\s+#\d+:\s+(\S+)") { $in = ($Matches[1] -eq $name); if ($l -match "Test\s+#\d+:\s+$([regex]::Escape($name))\s.*(Failed|Timeout|Exception)") { $in = $true } }
        if ($in -and $l -cmatch "FAIL|VUID|\[Vulkan\]|DEVICE_LOST|Timeout|Exception" -and $l -notmatch "Test\s+#") { return $l.Trim() } # (case sensitive: not "Stream failed ok")
    }
    return $first
}

Write-Host ""
Write-Host ("{0,-24} {1,-8} {2,7} {3,8}" -f "Area", "", "testes", "tempo")
$failedNames = @(); $anyFail = $false
foreach ($a in $areas) {
    $ts = @($tests | Where-Object { $_.area -eq $a })
    if (-not $ts) { continue }
    $ok = 0; $fail = @(); $skip = 0; $s = 0.0; $miss = 0
    foreach ($t in $ts) {
        $r = $res[$t.name]
        if (-not $r) { $miss++; $fail += $t.name; continue }
        $s += $r.secs
        if ($r.st -eq "ok") { $ok++ } elseif ($r.st -eq "skip") { $skip++ } else { $fail += $t.name }
    }
    $verdict = if ($fail) { "FALHOU" } else { "PASSOU" }
    $color = if ($fail) { "Red" } else { "Green" }
    $extra = if ($skip) { " ($skip pulado)" } else { "" }
    Write-Host ("{0,-24} {1,-8} {2,3}/{3,-3} {4,6:0}s{5}" -f $a, $verdict, $ok, $ts.Count, $s, $extra) -ForegroundColor $color
    foreach ($f in $fail) {
        $line = if ($res[$f]) { Get-FailLine $f } else { "(nao rodou)" }
        Write-Host ("    {0}: {1}" -f $f, $line)
        $failedNames += $f; $anyFail = $true
    }
}
$failedNames | Set-Content $failedFile -Encoding UTF8
Write-Host ""
Write-Host ("Tempo: {0}:{1:00}   Log: {2}" -f [int][math]::Floor($secs / 60), ($secs % 60), $log)
if ($leftAfter) { Write-Host ("{0} processo(s) do build ficaram vivos depois dos testes (fechados): um teste nao fecha o que abre." -f $leftAfter) -ForegroundColor Red; $anyFail = $true }
if ($anyFail) { Write-Host ("SUITE: FALHOU ({0} testes; tools/suite.ps1 -Falhas repete so eles)" -f $failedNames.Count) -ForegroundColor Red; exit 1 }
Write-Host "SUITE: PASSOU" -ForegroundColor Green
exit 0
