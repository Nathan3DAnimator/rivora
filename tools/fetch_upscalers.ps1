# Downloads the vendors' upscaler SDKs into external/upscalers (not versioned: binaries under their own licenses,
# notas-dev/upscaling.md). The build finds them there (CMakeLists.txt: RE_HAVE_DLSS / RE_HAVE_XESS / RE_HAVE_FSR) and
# copies the runtime DLLs beside the editor and the packaged game.
#   NVIDIA DLSS 310.9.16 (Super Resolution; the NVIDIA RTX SDKs license, LICENSE.txt)
#   Intel XeSS 3.0.2 (the Intel Simplified Software License)
#   AMD FidelityFX FSR 3.1.4 through the FidelityFX API (MIT; the signed Vulkan DLL)
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'external\upscalers'
New-Item -ItemType Directory -Force $root | Out-Null

function Get-File($url, $dest) {
    if ((Test-Path $dest) -and -not $Force) { return }
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Write-Host "  $url"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$dest.part"
    Move-Item -Force "$dest.part" $dest
}

# DLSS: the headers, the NGX static library (dynamic CRT) and the release DLL
$dlss = 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16'
Write-Host 'NVIDIA DLSS 310.9.16'
foreach ($h in 'nvsdk_ngx.h', 'nvsdk_ngx_defs.h', 'nvsdk_ngx_params.h', 'nvsdk_ngx_helpers.h', 'nvsdk_ngx_vk.h', 'nvsdk_ngx_defs_vk.h',
               'nvsdk_ngx_helpers_vk.h') {
    Get-File "$dlss/include/$h" "$root\dlss\include\$h"
}
Get-File "$dlss/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" "$root\dlss\lib\nvsdk_ngx_d.lib"
Get-File "$dlss/lib/Windows_x86_64/rel/nvngx_dlss.dll" "$root\dlss\bin\nvngx_dlss.dll"
Get-File "$dlss/LICENSE.txt" "$root\dlss\LICENSE.txt"

# FSR 3.1.4: the FidelityFX API headers and the signed Vulkan DLL (loaded at run time: no import library)
$ffx = 'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4'
Write-Host 'AMD FSR 3.1.4'
foreach ($h in 'ffx_api.h', 'ffx_api_types.h', 'ffx_api_loader.h', 'ffx_upscale.h', 'vk/ffx_api_vk.h') {
    Get-File "$ffx/ffx-api/include/ffx_api/$h" "$root\fsr\include\ffx_api\$($h -replace '/', '\')"
}
Get-File "$ffx/PrebuiltSignedDLL/amd_fidelityfx_vk.dll" "$root\fsr\bin\amd_fidelityfx_vk.dll"
Get-File "$ffx/LICENSE.txt" "$root\fsr\LICENSE.txt"

# XeSS 3.0.2: the SDK's zip, only its headers, DLLs and license kept
Write-Host 'Intel XeSS 3.0.2'
if (-not (Test-Path "$root\xess\bin\libxess.dll") -or $Force) {
    $zip = Join-Path $root 'xess_sdk.zip'
    Get-File 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip' $zip
    $tmp = Join-Path $root 'xess_unzip'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -Path $zip -DestinationPath $tmp
    $inc = Get-ChildItem -Recurse -Path $tmp -Filter 'xess_vk.h' | Select-Object -First 1
    $dll = Get-ChildItem -Recurse -Path $tmp -Filter 'libxess.dll' | Where-Object { $_.FullName -notmatch 'samples' } | Select-Object -First 1
    if (-not $inc -or -not $dll) { throw 'XeSS: xess_vk.h or libxess.dll not found in the SDK zip' }
    New-Item -ItemType Directory -Force "$root\xess\include\xess", "$root\xess\bin" | Out-Null
    Copy-Item -Force "$($inc.DirectoryName)\*.h" "$root\xess\include\xess\"
    Copy-Item -Force $dll.FullName "$root\xess\bin\"
    Remove-Item -Recurse -Force $tmp
    Remove-Item -Force $zip
}
Get-File 'https://raw.githubusercontent.com/intel/xess/v3.0.2/LICENSE.txt' "$root\xess\LICENSE.txt"
Write-Host "OK: $root"
