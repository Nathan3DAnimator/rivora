# Packages a build of the Rivora Engine (Real Engine until 0.49; copied from the Tlender's tools/make_dist.ps1 and adapted): builds Release in
# build-release with the version of VERSION (-Beta adds "_b": "faz a build" without saying = beta) and writes
# dist/Rivora-<version>-win64/ + .zip at the repository root (exe, pdb, updater, the Visual C++ runtime, LEIA-ME.txt).
#   powershell -ExecutionPolicy Bypass -File tools/make_dist.ps1 [-Beta] [-SignKey <private key file>]
param([switch]$Beta,
      [string]$SignKey) # signs the zip (rivora_updater --sign: <zip>.sig, <zip>.sha256)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot          # RealEngine/
$verText = (Get-Content (Join-Path $root "VERSION") -TotalCount 1).Trim()
if ($Beta) { $verText = "${verText}_b" }
Write-Host "Rivora Engine $verText"

# 1. Release build (the version goes in with -DRE_VERSION_STRING)
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$build = Join-Path $root "build-release"
& $cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 "-DRE_VERSION_STRING=$verText"
if ($LASTEXITCODE -ne 0) { throw "cmake configure falhou" }
$env:CL = "/MP"
& $cmake --build $build --config Release --target realengine realengine_updater realengine_game realengine_legacy -- /m /v:m
if ($LASTEXITCODE -ne 0) { throw "a build falhou" }
$rel = Join-Path $build "Release"

# 2. The package folder
$name = "Rivora-$verText-win64"
$dist = Join-Path $root "dist"
$dir = Join-Path $dist $name
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item (Join-Path $rel "rivora.exe") $dir
Copy-Item (Join-Path $rel "rivora.pdb") $dir # the crash reports name the functions with it (crashlog.c)
Copy-Item (Join-Path $rel "rivora_updater.exe") $dir # installs the updates after the editor closes (updater.h)
Copy-Item (Join-Path $rel "rivora_game.exe") $dir # Build > Package Game copies it as the game (editor_package_game)
# (2026-11-23, Real Engine -> Rivora Engine) realengine.exe: the launcher that starts rivora.exe (src/legacy_launcher.c): the
# older installs' shortcuts and updater open it
Copy-Item (Join-Path $rel "realengine.exe") $dir
# (2026-11-23, the open source release) the license, the third-party notices (tools/make_notices.py), the privacy note and
# the readme beside the editor; Package Game copies the first two next to every game
foreach ($f in "LICENSE", "THIRD_PARTY_NOTICES.md", "PRIVACY.md", "README.md") { Copy-Item (Join-Path $root $f) $dir }
# the plugins next to the editor (Plugins/ExamplePlugin: its DLL, plugin.json, source) and the plugin API's header (SDK/:
# Edit > Plugins > New Plugin copies it beside a new plugin's source); built with the editor (B14c, notas-dev/editor.md)
foreach ($d in "Plugins", "SDK") {
    if (Test-Path (Join-Path $rel $d)) { Copy-Item (Join-Path $rel $d) $dir -Recurse }
    else { Write-Warning "$d ausente na build: o pacote sai sem ele" }
}
# the demos' CC0 assets (Poly Haven, ambientCG: DemoContent/LICENSES.txt), fetched by the build (tools/fetch_demo_assets.cmake)
if (Test-Path (Join-Path $rel "DemoContent")) { Copy-Item (Join-Path $rel "DemoContent") $dir -Recurse }
# Build > Package for Android (notas-dev/android.md): the packager, the activity, the manifest and the prebuilt arm64
# runtime (android/prebuilt: librealengine_game.so, classes.dex; made by package_apk.ps1 -BuildNative)
$androidSrc = Join-Path $PSScriptRoot "..\android"
$androidDst = Join-Path $dir "android"
New-Item -ItemType Directory -Force $androidDst | Out-Null
foreach ($f in "package_apk.ps1", "AndroidManifest.xml") { Copy-Item (Join-Path $androidSrc $f) $androidDst }
Copy-Item (Join-Path $androidSrc "java") $androidDst -Recurse
Copy-Item (Join-Path $root "data\branding\logo.png") (Join-Path $androidDst "logo.png") # (the launcher icon when the project has none: the exe embeds data/)
if (Test-Path (Join-Path $androidSrc "prebuilt\librealengine_game.so")) { Copy-Item (Join-Path $androidSrc "prebuilt") $androidDst -Recurse }
else { Write-Warning "android/prebuilt sem librealengine_game.so: rode android/package_apk.ps1 -BuildNative antes (o pacote sai sem Android)" }
$crt = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory |
    Sort-Object FullName | Select-Object -Last 1
foreach ($d in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll") { Copy-Item (Join-Path $crt.FullName $d) $dir }
# the vendors' upscalers (tools/fetch_upscalers.ps1; notas-dev/upscaling.md): their runtimes and licenses beside the editor
foreach ($d in "nvngx_dlss.dll", "nvngx_dlss_LICENSE.txt", "amd_fidelityfx_vk.dll", "amd_fidelityfx_LICENSE.txt", "libxess.dll", "libxess_LICENSE.txt") {
    if (Test-Path (Join-Path $rel $d)) { Copy-Item (Join-Path $rel $d) $dir }
    else { Write-Warning "$d ausente: rode tools/fetch_upscalers.ps1 e refaca a build (o pacote sai so com o TSR)" }
}

# LEIA-ME: UTF-8 with BOM and CRLF; the first line says the version and the commit
$commit = (& git -C $root rev-parse --short HEAD).Trim()
$date = Get-Date -Format "yyyy-MM-dd"
$body = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "dist_leiame.txt")) -replace "`r", ""
$text = "Rivora Engine $verText (Windows x64), build de main $commit ($date)`n`n$body"
[IO.File]::WriteAllText((Join-Path $dir "LEIA-ME.txt"), ($text -replace "`n", "`r`n"), (New-Object Text.UTF8Encoding $true))

# 3. The zip
$zip = Join-Path $dist "$name.zip"
Compress-Archive -Path $dir -DestinationPath $zip -Force
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Pacote: $zip ($mb MB)"

# 4. The signature (the updater installs only signed packages). Upload the zip, the .sig and the .sha256 to the
# GitHub release; the private key stays out of the repository.
if ($SignKey) {
    & (Join-Path $rel "rivora_updater.exe") --sign $SignKey $zip
    if ($LASTEXITCODE -ne 0) { throw "a assinatura falhou" }
    Write-Host "Assinado: $zip.sig, $zip.sha256"
}
# (2026-11-23) the same package under the old name too: the 0.49 and older updaters look for RealEngine-*-win64.zip (the
# signature is of its bytes, the same; the .sha256 names the file)
$old = Join-Path $dist ("RealEngine-$verText-win64.zip")
Copy-Item $zip $old -Force
if (Test-Path "$zip.sig") { Copy-Item "$zip.sig" "$old.sig" -Force }
if (Test-Path "$zip.sha256") { (Get-Content "$zip.sha256" -Raw).Replace("$name.zip", "RealEngine-$verText-win64.zip") | Set-Content "$old.sha256" -NoNewline -Encoding ascii }
Write-Host "Também como: $old (para os atualizadores da 0.49 e anteriores)"
