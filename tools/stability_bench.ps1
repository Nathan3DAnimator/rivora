<#
  (Q20) GI and reflections' stability, measured (notas-dev/gi-estavel.md): each preset (Low..Epic, and the Photoreal style
  with the denoiser) in three views of the house:
    still   the camera still                      -> flicker (the mean change frame to frame)
    pan     the camera moving slowly (--cam-move) -> flicker back and forth (what moves smoothly doesn't count)
    ghost   a pillar sweeping across the view, the camera still (--scene house_ghost) -> ghosting (the trail behind it)
  plus fps and the GPU's least frame. Prints one table row per run.

    tools/stability_bench.ps1                         every preset, the three views
    tools/stability_bench.ps1 -Presets 3 -Styles 2    Epic, Photoreal only
    tools/stability_bench.ps1 -Env REALENGINE_NO_Q20_GHOST=1   an A/B against a switch

  Windows on the 2nd monitor (REALENGINE_MONITOR=2); -Hidden runs them off screen (REALENGINE_BACKGROUND=1).
#>
param(
    [int[]]$Presets = @(0, 1, 2, 3),
    [int[]]$Styles = @(1, 2),
    [string[]]$Views = @("still", "pan", "ghost"),
    [string[]]$Env = @(),
    [string]$Build = "build",
    [double]$Seconds = 3,
    [int]$Runs = 1,
    [switch]$Hidden
)
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "$Build\Release\realengine.exe"
if (-not (Test-Path $exe)) { Write-Host "sem $exe"; exit 2 }
if ($Hidden) { $env:REALENGINE_BACKGROUND = "1"; Remove-Item Env:REALENGINE_MONITOR -ErrorAction SilentlyContinue }
else { $env:REALENGINE_MONITOR = "2"; Remove-Item Env:REALENGINE_BACKGROUND -ErrorAction SilentlyContinue }
foreach ($e in $Env) { $kv = $e -split "=", 2; Set-Item "Env:$($kv[0])" $kv[1] }
$pnames = @("Baixo", "Medio", "Alto", "Epico")
$snames = @{ 1 = "Realista"; 2 = "Fotorrealista"; 3 = "Video" }
"{0,-13} {1,-6} {2,-5} {3,7} {4,7} {5,8} {6,8} {7,8} {8,8}" -f "estilo", "preset", "vista", "fps", "GPUmin", "flicker", "vaivem", "ghost", "fundo"
foreach ($s in $Styles) {
    foreach ($p in $Presets) {
        foreach ($v in $Views) {
            for ($r = 0; $r -lt $Runs; $r++) {
                $scene = if ($v -eq "ghost") { "house_ghost" } else { "house" }
                $a = @("--scene", $scene, "--render-style", "$s", "--scalability", "$p", "--bench", "$Seconds")
                if ($v -eq "pan") { $a += @("--cam-move", "-25,-12,0") }
                $out = & $exe @a 2>&1 | Out-String
                $fps = if ($out -match "Measure: (\d+) fps") { $Matches[1] } else { "?" }
                $gm = if ($out -match "GPU minimum: ([\d.]+) ms") { $Matches[1] } else { "?" }
                $fl = if ($out -match "flicker ([\d.]+)%") { $Matches[1] } else { "?" }
                $pu = if ($out -match "back and forth: ([\d.]+)%") { $Matches[1] } else { "?" }
                $gh = if ($out -match "Ghosting: ([\d.]+)% behind moving actors \((\d+) points") { "$($Matches[1])" } else { "?" }
                $bg = if ($out -match "background ([\d.]+)%") { $Matches[1] } else { "?" }
                "{0,-13} {1,-6} {2,-5} {3,7} {4,7} {5,8} {6,8} {7,8} {8,8}" -f $snames[$s], $pnames[$p], $v, $fps, $gm, $fl, $pu, $gh, $bg
                if ($out -match "FAIL|VUID") { ($out -split "`n" | Where-Object { $_ -match "FAIL|VUID" } | Select-Object -First 3) | ForEach-Object { "   $_" } }
            }
        }
    }
}
