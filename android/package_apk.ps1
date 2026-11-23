# The Android game (notas-dev/android.md): a packaged game's folder (Build > Package Game's: game.ini, Content, the
# levels) into a signed APK (to install and test) and/or an Android App Bundle (.aab, what Google Play takes), without
# Gradle or Android Studio.
#
#   powershell -ExecutionPolicy Bypass -File android/package_apk.ps1 -GameDir <folder> -Out <file.apk|file.aab>
#       [-Format apk|aab|both] [-Package com.company.game] [-Title "My Game"] [-Orientation sensorLandscape|...]
#       [-VersionName 1.0] [-VersionCode 1] [-TargetSdk 36] [-Icon <png>] [-IconBackground '#101418']
#       [-Keystore <file> -KeyAlias <alias> -KeyPassFile <file>] [-AssetPack auto|on|off] [-StoreAssets <folder>]
#       [-FeatureImage <png>] [-AR] [-Quest [-HandTracking] [-MixedReality]] [-Install] [-BuildNative]
#
# One module for both: aapt2 links the manifest and the resources in proto form (the bundle's); the APK is that module
# converted to binary, so installing the APK tests what goes to the store. The bundle: BundleConfig.pb + base/ (manifest,
# resources.pb, res, dex, lib, assets) and, past 150 MB of game files, an install-time asset pack (game_assets/: Play's
# 200 MB limit is the base's; the pack's files reach the same AssetManager). Signed with jarsigner (the upload key).
# The library's segments are 16 KB aligned (Android 15's 16 KB pages: linked with max-page-size=16384).
#
# The engine's parts: librealengine_game.so (arm64-v8a) and classes.dex - next to this script in prebuilt/ (the engine's
# release ships them), else built here from the sources (-BuildNative, or prebuilt/ missing: the NDK and the Vulkan SDK).
# Needs the Android SDK (build-tools, a platform) and a JDK: %LOCALAPPDATA%\Android\Sdk or ANDROID_HOME, JAVA_HOME.
# -AR (Project Settings > Android > Augmented Reality): ARCore (src/ar_arcore.c) goes in: its AAR downloaded once from
# Google's Maven into %APPDATA%\RealEngine\android\arcore\<version> (its libraries into lib/arm64-v8a, its classes into
# classes2.dex), the camera's permission, "AR optional" (the game runs on any phone; AR where Google Play Services for AR
# is installed). Its InstallActivity is declared (ARCore checks the manifest for it) but never started: its resources
# stay out; the game offers the store's page.
# Without -Keystore: a debug key made once in %APPDATA%\RealEngine\android (installs on your devices; Google Play takes
# a bundle signed with your upload key: Project Settings > Android > Create Upload Key).
param(
    [string]$GameDir = '',
    [string]$Out = '',
    [string]$NewUploadKey = '',   # only make an upload key there (its password beside it, .pass) and stop
    [string]$KeyOwner = 'Real Engine Game',
    [ValidateSet('apk', 'aab', 'both')][string]$Format = '',
    [string]$Package = 'com.mycompany.mygame',
    [string]$Title = 'My Game',
    [string]$Orientation = 'sensorLandscape',
    [string]$VersionName = '1.0',
    [int]$VersionCode = 1,
    [int]$TargetSdk = 36,
    [string]$Icon = '',
    [string]$IconBackground = '#101418',
    [string]$Keystore = '',
    [string]$KeyAlias = 'game',
    [string]$KeyPassFile = '',
    [ValidateSet('auto', 'on', 'off')][string]$AssetPack = 'auto',
    [string]$StoreAssets = '',
    [string]$FeatureImage = '',
    [switch]$AR,
    [switch]$ARNoOcclusion,
    [switch]$Quest,          # (Q25) a Meta Quest VR app: the OpenXR loader, Meta's VR manifest, landscape, game.ini's vr=1
    [switch]$HandTracking,   # (Q25) with -Quest: the hand tracking's permission
    [switch]$MixedReality,   # (Q25b) with -Quest: the passthrough's feature and the Scene permission (the room, the real depth)
    [switch]$Install,
    [switch]$BuildNative
)
$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {} # (the editor's log reads UTF-8)
if (-not $Format) { $Format = if ($Out -match '\.aab$') { 'aab' } else { 'apk' } }
$apkOut = if ($Out -match '\.aab$') { [IO.Path]::ChangeExtension($Out, '.apk') } else { $Out }
$aabOut = if ($Out -match '\.aab$') { $Out } else { [IO.Path]::ChangeExtension($Out, '.aab') }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path      # the engine's android folder
$engine = Split-Path -Parent $here
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) { throw "Android SDK não encontrado ($sdk): instale o Android SDK (cmdline-tools: build-tools e uma platform)" }
$bt = Get-ChildItem (Join-Path $sdk 'build-tools') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$platform = Get-ChildItem (Join-Path $sdk 'platforms') -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $bt -or -not $platform) { throw 'Android SDK sem build-tools ou platform (sdkmanager "build-tools;35.0.0" "platforms;android-35")' }
$androidJar = Join-Path $platform.FullName 'android.jar'
if (-not $env:JAVA_HOME) {
    $jdk = Get-ChildItem 'C:\Program Files\Microsoft', 'C:\Program Files\Java', 'C:\Program Files\Eclipse Adoptium' -Directory -Filter 'jdk*' -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
}
if (-not $env:JAVA_HOME) { throw 'JDK não encontrado (JAVA_HOME): instale um JDK 17 ou mais novo' }
$keytool = Join-Path $env:JAVA_HOME 'bin\keytool.exe'

# --- The upload key (Google Play): made once, 27 years, RSA 4096; its password in a file beside it -------------------
if ($NewUploadKey) {
    if (Test-Path $NewUploadKey) { throw "Já existe uma chave em $NewUploadKey (nunca substitua a chave de upload)" }
    New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($NewUploadKey))) | Out-Null
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $pass = [Convert]::ToBase64String($bytes).Replace('+', 'a').Replace('/', 'b').Replace('=', '')
    [IO.File]::WriteAllText("$NewUploadKey.pass", $pass)
    $owner = $KeyOwner.Replace(',', ' ').Replace('=', ' ')
    & $keytool -genkeypair -keystore $NewUploadKey -storetype PKCS12 -storepass $pass -keypass $pass -alias upload -keyalg RSA `
        -keysize 4096 -validity 10000 -dname "CN=$owner, O=$owner" | Out-Null
    if ($LASTEXITCODE -ne 0) { Remove-Item -Force -ErrorAction SilentlyContinue "$NewUploadKey.pass"; throw 'keytool falhou' }
    Write-Host "Chave de upload: $NewUploadKey (alias upload). Guarde uma cópia dela e do .pass fora do PC."
    exit 0
}
if (-not $GameDir -or -not $Out) { throw 'Use -GameDir <pasta do jogo> -Out <arquivo.apk|.aab> (ou -NewUploadKey <arquivo>)' }
$aapt2 = Join-Path $bt.FullName 'aapt2.exe'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing

function Add-File($zip, $path, $entry, $store) {
    $level = if ($store) { [IO.Compression.CompressionLevel]::NoCompression } else { [IO.Compression.CompressionLevel]::Optimal }
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $path, $entry, $level)
}
function Add-Tree($zip, $dir, $prefix) { # (here, not by aapt2 -A: on Windows it writes backslashes)
    Get-ChildItem -Recurse -File $dir | ForEach-Object {
        Add-File $zip $_.FullName ($prefix + $_.FullName.Substring($dir.Length + 1).Replace('\', '/')) $false
    }
}
function Copy-Zip($from, $zip, $prefix, $skip) { # a zip's entries into another, renamed (the proto module into the bundle)
    $src = [IO.Compression.ZipFile]::OpenRead($from)
    foreach ($e in $src.Entries) {
        if ($e.FullName.EndsWith('/') -or ($skip -and $e.FullName -match $skip)) { continue }
        $name = $prefix + $(if ($e.FullName -eq 'AndroidManifest.xml') { 'manifest/AndroidManifest.xml' } else { $e.FullName })
        $dst = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
        $a = $e.Open(); $b = $dst.Open(); $a.CopyTo($b); $b.Dispose(); $a.Dispose()
    }
    $src.Dispose()
}
function Save-Png($img, $size, $path, $bg, $inner) { # a square picture: the image centered at inner (0-1) over bg
    $bmp = New-Object Drawing.Bitmap $size, $size
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear($bg)
    $box = $size * $inner
    $s = [math]::Min($box / $img.Width, $box / $img.Height)
    $w = [int]($img.Width * $s); $h = [int]($img.Height * $s)
    $g.DrawImage($img, [int](($size - $w) / 2), [int](($size - $h) / 2), $w, $h)
    $g.Dispose()
    $bmp.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# --- The engine's parts: prebuilt, else built ---------------------------------------------------------------------
$prebuilt = Join-Path $here 'prebuilt'
$so = Join-Path $prebuilt 'librealengine_game.so'
$dex = Join-Path $prebuilt 'classes.dex'
$stage = Join-Path ([IO.Path]::GetTempPath()) ("realengine_apk_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $stage | Out-Null
try {
if ($BuildNative -or -not (Test-Path $so)) {
    $ndk = Get-ChildItem (Join-Path $sdk 'ndk') -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $ndk) { throw "Sem librealengine_game.so pronta e sem NDK para compilar ($sdk\ndk)" }
    $vs = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake'
    if (-not (Test-Path $vs)) { $vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake' }
    $env:PATH = "$vs\CMake\bin;$vs\Ninja;$env:PATH"
    $vulkanSdk = if ($env:VULKAN_SDK) { $env:VULKAN_SDK } else { (Get-ChildItem 'C:\VulkanSDK' -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName }
    $build = Join-Path $engine 'build-android'
    cmake -G Ninja -S $engine -B $build "-DCMAKE_TOOLCHAIN_FILE=$($ndk.FullName)\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release "-DGLSLC=$vulkanSdk\Bin\glslc.exe" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake (configurar o Android) falhou' }
    cmake --build $build --target realengine_game
    if ($LASTEXITCODE -ne 0) { throw 'a compilação do librealengine_game.so falhou' }
    New-Item -ItemType Directory -Force $prebuilt | Out-Null
    $strip = Join-Path $ndk.FullName 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
    & $strip --strip-unneeded -o $so (Join-Path $build 'librealengine_game.so')
    if ($LASTEXITCODE -ne 0) { throw 'llvm-strip falhou' }
}
if ($BuildNative -or -not (Test-Path $dex)) {
    $classes = Join-Path $stage 'classes'
    New-Item -ItemType Directory -Force $classes | Out-Null
    $javac = Join-Path $env:JAVA_HOME 'bin\javac.exe'
    $javaFiles = @(Get-ChildItem -Recurse -Filter *.java (Join-Path $here 'java') | ForEach-Object { $_.FullName })
    & $javac --release 11 -classpath $androidJar -d $classes -nowarn @javaFiles
    if ($LASTEXITCODE -ne 0) { throw 'javac falhou' }
    $classFiles = @(Get-ChildItem -Recurse -Filter *.class $classes | ForEach-Object { $_.FullName })
    $dexDir = Join-Path $stage 'dex'
    New-Item -ItemType Directory -Force $dexDir | Out-Null
    & (Join-Path $bt.FullName 'd8.bat') --release --min-api 30 --lib $androidJar --output $dexDir @classFiles
    if ($LASTEXITCODE -ne 0) { throw 'd8 falhou' }
    New-Item -ItemType Directory -Force $prebuilt | Out-Null
    Copy-Item -Force (Join-Path $dexDir 'classes.dex') $dex
}

# --- ARCore (-AR): its AAR once, its two libraries, its classes dexed (classes2.dex) ---------------------------------
$arVersion = '1.57.0'
$arSo = @(); $arDex = $null
if ($AR) {
    $arDir = Join-Path $env:APPDATA "RealEngine\android\arcore\$arVersion"
    $aar = Join-Path $arDir "core-$arVersion.aar"
    if (-not (Test-Path $aar)) {
        New-Item -ItemType Directory -Force $arDir | Out-Null
        Write-Host "ARCore ${arVersion}: baixando do Maven do Google"
        Invoke-WebRequest -UseBasicParsing -Uri "https://dl.google.com/dl/android/maven2/com/google/ar/core/$arVersion/core-$arVersion.aar" -OutFile "$aar.part"
        Move-Item -Force "$aar.part" $aar
    }
    $arX = Join-Path $arDir 'x'
    if (-not (Test-Path (Join-Path $arX 'classes.jar'))) {
        if (Test-Path $arX) { Remove-Item -Recurse -Force $arX }
        [IO.Compression.ZipFile]::ExtractToDirectory($aar, $arX)
    }
    $arSo = @((Join-Path $arX 'jni\arm64-v8a\libarcore_sdk_c.so'), (Join-Path $arX 'jni\arm64-v8a\libarcore_sdk_jni.so'))
    $arDex = Join-Path $arDir 'classes2.dex'
    if (-not (Test-Path $arDex)) {
        $dd = Join-Path $stage 'ardex'
        New-Item -ItemType Directory -Force $dd | Out-Null
        # (its R classes are not there: only ARCore's install screen uses them, and it is left out)
        & (Join-Path $bt.FullName 'd8.bat') --release --min-api 30 --lib $androidJar --output $dd (Join-Path $arX 'classes.jar')
        if ($LASTEXITCODE -ne 0) { throw 'd8 (ARCore) falhou' }
        Copy-Item -Force (Join-Path $dd 'classes.dex') $arDex
    }
}
$arManifest = if ($AR) { @'
    <uses-permission android:name="android.permission.CAMERA" />
    <uses-feature android:name="android.hardware.camera.ar" android:required="false" />
    <queries>
        <package android:name="com.google.ar.core" />
        <package android:name="com.android.vending" />
    </queries>
'@ } else { '' }
$arApplication = if ($AR) { @'
        <meta-data android:name="com.google.ar.core" android:value="optional" />
        <meta-data android:name="com.google.ar.core.min_apk_version" android:value="262070000" />
        <activity android:name="com.google.ar.core.InstallActivity" android:configChanges="keyboardHidden|orientation|screenSize"
            android:excludeFromRecents="true" android:exported="false" android:launchMode="singleTop"
            android:theme="@android:style/Theme.Material.Light.Dialog.Alert" />
'@ } else { '' }

# --- Meta Quest (-Quest, Q25; notas-dev/xr.md): the Khronos OpenXR loader for Android (Apache-2.0: the headset's runtime
# is found through it) once from Maven Central into %APPDATA%\RealEngine\android\openxr\<version>; its library into
# lib/arm64-v8a. The manifest: head tracking required, Meta's VR category and the OpenXR one, the devices, focus aware;
# the runtime broker's queries (the loader asks the system which runtime is active). Landscape; game.ini's vr=1 ------------
$xrSo = $null
if ($Quest) {
    $Orientation = 'landscape'
    $xrRoot = Join-Path $env:APPDATA 'RealEngine\android\openxr'
    $find = { Get-ChildItem -Recurse -File -Filter 'libopenxr_loader.so' -Path $xrRoot -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'arm64-v8a' } }
    $found = @(& $find)
    if ($found.Count -eq 0) {
        foreach ($v in @('1.1.49', '1.1.48', '1.1.47', '1.1.46', '1.1.45', '1.1.43', '1.1.41', '1.1.38', '1.1.36', '1.0.34')) {
            $dir = Join-Path $xrRoot $v
            $aarX = Join-Path $dir "openxr_loader_for_android-$v.aar"
            New-Item -ItemType Directory -Force $dir | Out-Null
            try {
                Write-Host "OpenXR loader ${v}: baixando do Maven Central (Khronos, Apache-2.0)"
                Invoke-WebRequest -UseBasicParsing -Uri "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$v/openxr_loader_for_android-$v.aar" -OutFile "$aarX.part"
                Move-Item -Force "$aarX.part" $aarX
                [IO.Compression.ZipFile]::ExtractToDirectory($aarX, (Join-Path $dir 'x'))
                break
            } catch {
                Remove-Item -Force -ErrorAction SilentlyContinue "$aarX.part"
            }
        }
        $found = @(& $find)
    }
    if ($found.Count -eq 0) { throw 'o loader OpenXR do Android nao foi baixado (sem internet? Maven Central)' }
    $xrSo = $found[0].FullName
    Write-Host "Meta Quest: $xrSo"
}
$vrManifest = ''
$vrApplication = ''
$vrActivity = ''
$vrCategory = ''
if ($Quest) {
    $vrManifest = @'
    <uses-feature android:name="android.hardware.vr.headtracking" android:required="true" android:version="1" />
    <uses-permission android:name="org.khronos.openxr.permission.OPENXR" />
    <uses-permission android:name="org.khronos.openxr.permission.OPENXR_SYSTEM" />
    <queries>
        <provider android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker" />
        <intent><action android:name="org.khronos.openxr.OpenXRRuntimeService" /></intent>
        <intent><action android:name="org.khronos.openxr.OpenXRApiLayerService" /></intent>
    </queries>
'@
    if ($HandTracking) {
        $vrManifest += "`n" + @'
    <uses-permission android:name="com.oculus.permission.HAND_TRACKING" />
    <uses-feature android:name="oculus.software.handtracking" android:required="false" />
'@
    }
    if ($MixedReality) { # (Q25b: src/xr_mr.c) the passthrough (not required: a Quest 2 plays it as VR), the room and the real depth's permission
        $vrManifest += "`n" + @'
    <uses-feature android:name="com.oculus.feature.PASSTHROUGH" android:required="false" />
    <uses-permission android:name="com.oculus.permission.USE_SCENE" />
'@
    }
    $vrApplication = @'
        <meta-data android:name="com.oculus.supportedDevices" android:value="quest2|questpro|quest3|quest3s" />
        <meta-data android:name="com.oculus.handtracking.version" android:value="V2.0" />
'@
    $vrActivity = '            <meta-data android:name="com.oculus.vr.focusaware" android:value="true" />'
    $vrCategory = @'
                <category android:name="com.oculus.intent.category.VR" />
                <category android:name="org.khronos.openxr.intent.category.IMMERSIVE_HMD" />
'@
}

# --- The game's files as assets/game/... and their list (the list in the base: small) --------------------------------
$assets = Join-Path $stage 'assets'
$gameAssets = Join-Path $assets 'game'
New-Item -ItemType Directory -Force $gameAssets | Out-Null
Get-ChildItem -Path $GameDir -Force | Where-Object { $_.Name -notmatch '\.(exe|dll|pdb|log)$' } | ForEach-Object {
    Copy-Item -Recurse -Force $_.FullName (Join-Path $gameAssets $_.Name)
}
if ($AR) { # (the game starts ARCore's session: game.ini's ar=1, whatever the project's file says)
    $ini = Join-Path $gameAssets 'game.ini'
    $arIni = if ($ARNoOcclusion) { "ar=1`nar_occlusion=0`n" } else { "ar=1`n" } # (the real things hiding the virtual ones: on unless asked)
    if (Test-Path $ini) { [IO.File]::AppendAllText($ini, "`n$arIni") } else { [IO.File]::WriteAllText($ini, $arIni) }
}
if ($Quest) { # (Q25: the game opens on the headset: game.ini's vr=1, whatever Start in VR says)
    $ini = Join-Path $gameAssets 'game.ini'
    if (Test-Path $ini) { [IO.File]::AppendAllText($ini, "`nvr=1`n") } else { [IO.File]::WriteAllText($ini, "vr=1`n") }
}
$files = @(Get-ChildItem -Recurse -File $gameAssets)
$list = $files | ForEach-Object { $_.FullName.Substring($assets.Length + 1).Replace('\', '/') }
$gameBytes = ($files | Measure-Object -Sum Length).Sum
$baseMeta = Join-Path $stage 'assets_base'
New-Item -ItemType Directory -Force $baseMeta | Out-Null
[IO.File]::WriteAllLines((Join-Path $baseMeta 'manifest.txt'), $list)
[IO.File]::WriteAllText((Join-Path $baseMeta 'build.txt'), "$VersionName-$VersionCode-" + (Get-Date -Format 'yyyyMMddHHmmss'))
$usePack = ($AssetPack -eq 'on') -or ($AssetPack -eq 'auto' -and $gameBytes -gt 150MB)

# --- The launcher icon: adaptive (Android 8+: the picture in the safe circle over a color) and the legacy one --------
$res = Join-Path $stage 'res'
$iconPath = if ($Icon -and (Test-Path $Icon)) { $Icon } elseif (Test-Path (Join-Path $here 'logo.png')) { Join-Path $here 'logo.png' } else { Join-Path $engine 'data\branding\logo.png' } # (the engine's package: android/logo.png)
$logo = [Drawing.Image]::FromFile($iconPath)
$bgColor = [Drawing.ColorTranslator]::FromHtml($IconBackground)
foreach ($d in @(@('mdpi', 48), @('hdpi', 72), @('xhdpi', 96), @('xxhdpi', 144), @('xxxhdpi', 192))) {
    $dir = Join-Path $res "mipmap-$($d[0])"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Save-Png $logo $d[1] (Join-Path $dir 'ic_launcher.png') ([Drawing.Color]::Transparent) 1.0
    Save-Png $logo ([int]($d[1] * 108 / 48)) (Join-Path $dir 'ic_launcher_foreground.png') ([Drawing.Color]::Transparent) 0.6 # (72 of 108 dp shown, a circle's cut)
}
$anydpi = Join-Path $res 'mipmap-anydpi-v26'
New-Item -ItemType Directory -Force $anydpi | Out-Null
[IO.File]::WriteAllText((Join-Path $anydpi 'ic_launcher.xml'), @'
<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
</adaptive-icon>
'@)
$values = Join-Path $res 'values'
New-Item -ItemType Directory -Force $values | Out-Null
$hex = '#{0:X2}{1:X2}{2:X2}' -f $bgColor.R, $bgColor.G, $bgColor.B
[IO.File]::WriteAllText((Join-Path $values 'colors.xml'), "<?xml version=`"1.0`" encoding=`"utf-8`"?>`n<resources><color name=`"ic_launcher_background`">$hex</color></resources>`n")
# Google Play's listing pictures (Play Console > Main store listing): the 512 px icon and the 1024 x 500 feature graphic
if ($StoreAssets) {
    New-Item -ItemType Directory -Force $StoreAssets | Out-Null
    Save-Png $logo 512 (Join-Path $StoreAssets 'play_icon_512.png') $bgColor 0.66
    $fg = New-Object Drawing.Bitmap 1024, 500
    $g = [Drawing.Graphics]::FromImage($fg)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear($bgColor)
    if ($FeatureImage -and (Test-Path $FeatureImage)) { # the project's loading picture, cropped to fill
        $pic = [Drawing.Image]::FromFile($FeatureImage)
        $s = [math]::Max(1024 / $pic.Width, 500 / $pic.Height)
        $w = [int]($pic.Width * $s); $h = [int]($pic.Height * $s)
        $g.DrawImage($pic, [int]((1024 - $w) / 2), [int]((500 - $h) / 2), $w, $h)
        $pic.Dispose()
    } else {
        $s = [math]::Min(300 / $logo.Width, 300 / $logo.Height)
        $g.DrawImage($logo, 80, [int]((500 - $logo.Height * $s) / 2), [int]($logo.Width * $s), [int]($logo.Height * $s))
        $font = New-Object Drawing.Font 'Segoe UI Semibold', 54
        $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $g.DrawString($Title, $font, [Drawing.Brushes]::White, 420, 205)
        $font.Dispose()
    }
    $g.Dispose()
    $fg.Save((Join-Path $StoreAssets 'play_feature_1024x500.png'), [Drawing.Imaging.ImageFormat]::Png)
    $fg.Dispose()
}
$logo.Dispose()

# --- The module: manifest and resources linked in proto form (the bundle's), converted to binary for the APK ----------
$manifest = Join-Path $stage 'AndroidManifest.xml'
$xmlTitle = [Security.SecurityElement]::Escape($Title)
(Get-Content -Raw (Join-Path $here 'AndroidManifest.xml')).Replace('@TITLE@', $xmlTitle).Replace('@ORIENTATION@', $Orientation).Replace('@AR_MANIFEST@', $arManifest).Replace('@AR_APPLICATION@', $arApplication).Replace('@VR_MANIFEST@', $vrManifest).Replace('@VR_APPLICATION@', $vrApplication).Replace('@VR_ACTIVITY@', $vrActivity).Replace('@VR_CATEGORY@', $vrCategory) |
    Set-Content -Encoding utf8 $manifest
$resZip = Join-Path $stage 'res.zip'
& $aapt2 compile --dir $res -o $resZip
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile falhou' }
$proto = Join-Path $stage 'base_proto.apk'
& $aapt2 link --proto-format -o $proto -I $androidJar --manifest $manifest -R $resZip --rename-manifest-package $Package `
    --version-code $VersionCode --version-name $VersionName --min-sdk-version 30 --target-sdk-version $TargetSdk --auto-add-overlay
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link falhou (o nome do pacote é válido? ex.: com.empresa.jogo)' }

# --- Signing: the project's key, else a debug key (made once) -------------------------------------------------------
if (-not $Keystore) {
    $keyDir = Join-Path $env:APPDATA 'RealEngine\android'
    $Keystore = Join-Path $keyDir 'debug.keystore'
    $KeyPassFile = Join-Path $keyDir 'debug.keystore.pass'
    $KeyAlias = 'debug'
    if (-not (Test-Path $Keystore)) {
        New-Item -ItemType Directory -Force $keyDir | Out-Null
        $bytes = New-Object byte[] 24
        [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
        $pass = [Convert]::ToBase64String($bytes).Replace('+', 'a').Replace('/', 'b').Replace('=', '')
        [IO.File]::WriteAllText($KeyPassFile, $pass)
        & $keytool -genkeypair -keystore $Keystore -storepass $pass -keypass $pass -alias debug -keyalg RSA -keysize 2048 `
            -validity 10000 -dname 'CN=Real Engine Debug, O=Real Engine' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'keytool falhou' }
    }
    if ($Format -ne 'apk') { Write-Warning 'O AAB saiu com a chave de depuração: a Play Store recusa. Crie a chave de upload (Project Settings > Android).' }
}
if (-not (Test-Path $Keystore)) { throw "Keystore não encontrada: $Keystore" }
if (-not (Test-Path $KeyPassFile)) { throw "Arquivo da senha da keystore não encontrado: $KeyPassFile" }
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null

# --- The APK (install and test): the module in binary, the dex, the library stored and 16 KB aligned, every asset ------
if ($Format -ne 'aab') {
    $unsigned = Join-Path $stage 'unsigned.apk'
    & $aapt2 convert --output-format binary -o $unsigned $proto
    if ($LASTEXITCODE -ne 0) { throw 'aapt2 convert falhou' }
    $zip = [IO.Compression.ZipFile]::Open($unsigned, [IO.Compression.ZipArchiveMode]::Update)
    Add-Tree $zip $assets 'assets/'
    Add-Tree $zip $baseMeta 'assets/'
    Add-File $zip $dex 'classes.dex' $false
    Add-File $zip $so 'lib/arm64-v8a/librealengine_game.so' $false # (extracted on install: its ELF has 16 KB pages)
    foreach ($f in $arSo) { Add-File $zip $f ('lib/arm64-v8a/' + (Split-Path -Leaf $f)) $false }
    if ($xrSo) { Add-File $zip $xrSo 'lib/arm64-v8a/libopenxr_loader.so' $false } # (Q25: the Quest's OpenXR loader)
    if ($arDex) { Add-File $zip $arDex 'classes2.dex' $false }
    $zip.Dispose()
    $aligned = Join-Path $stage 'aligned.apk'
    & (Join-Path $bt.FullName 'zipalign.exe') -f -P 16 4 $unsigned $aligned
    if ($LASTEXITCODE -ne 0) { throw 'zipalign falhou' }
    & (Join-Path $bt.FullName 'apksigner.bat') sign --ks $Keystore --ks-key-alias $KeyAlias --ks-pass "file:$KeyPassFile" --out $apkOut $aligned
    if ($LASTEXITCODE -ne 0) { throw 'apksigner falhou (a chave, o alias e a senha conferem?)' }
    Remove-Item -Force -ErrorAction SilentlyContinue "$apkOut.idsig"
    $mb = [math]::Round((Get-Item $apkOut).Length / 1MB, 1)
    Write-Host "APK: $apkOut ($mb MB)"
}

# --- The bundle (Google Play): BundleConfig, base/ and the asset pack, signed with jarsigner --------------------------
if ($Format -ne 'apk') {
    if (Test-Path $aabOut) { Remove-Item -Force $aabOut }
    $cfg = Join-Path $stage 'BundleConfig.pb'
    # BundleConfig { bundletool { version: "1.17.2" } } (config.proto: field 1, its version field 2): what Play reads
    $ver = [Text.Encoding]::ASCII.GetBytes('1.17.2')
    $pb = [byte[]](@(0x0A, ($ver.Length + 2), 0x12, $ver.Length) + $ver)
    [IO.File]::WriteAllBytes($cfg, $pb)
    $zip = [IO.Compression.ZipFile]::Open($aabOut, [IO.Compression.ZipArchiveMode]::Create)
    Add-File $zip $cfg 'BundleConfig.pb' $false
    Copy-Zip $proto $zip 'base/' '^META-INF/'
    Add-File $zip $dex 'base/dex/classes.dex' $false
    Add-File $zip $so 'base/lib/arm64-v8a/librealengine_game.so' $false
    foreach ($f in $arSo) { Add-File $zip $f ('base/lib/arm64-v8a/' + (Split-Path -Leaf $f)) $false }
    if ($xrSo) { Add-File $zip $xrSo 'base/lib/arm64-v8a/libopenxr_loader.so' $false }
    if ($arDex) { Add-File $zip $arDex 'base/dex/classes2.dex' $false }
    Add-Tree $zip $baseMeta 'base/assets/'
    if ($usePack) { # past 150 MB: the game's files in an install-time asset pack (installed with the app, the same AssetManager)
        $packManifest = Join-Path $stage 'pack_manifest.xml'
        [IO.File]::WriteAllText($packManifest, @"
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" xmlns:dist="http://schemas.android.com/apk/distribution"
    package="$Package" split="game_assets">
    <dist:module dist:type="asset-pack">
        <dist:fusing dist:include="true" />
        <dist:delivery><dist:install-time /></dist:delivery>
    </dist:module>
</manifest>
"@)
        $packProto = Join-Path $stage 'pack_proto.apk'
        & $aapt2 link --proto-format -o $packProto -I $androidJar --manifest $packManifest --min-sdk-version 30 --target-sdk-version $TargetSdk
        if ($LASTEXITCODE -ne 0) { throw 'aapt2 link (asset pack) falhou' }
        Copy-Zip $packProto $zip 'game_assets/' '^(META-INF/|resources\.pb$)'
        Add-Tree $zip $assets 'game_assets/assets/'
    } else {
        Add-Tree $zip $assets 'base/assets/'
    }
    $zip.Dispose()
    $jarsigner = Join-Path $env:JAVA_HOME 'bin\jarsigner.exe'
    & $jarsigner -keystore $Keystore -storepass:file $KeyPassFile -keypass:file $KeyPassFile -sigalg SHA256withRSA -digestalg SHA-256 $aabOut $KeyAlias | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'jarsigner falhou (a chave, o alias e a senha conferem?)' }
    $mb = [math]::Round((Get-Item $aabOut).Length / 1MB, 1)
    $packNote = if ($usePack) { ', arquivos do jogo num asset pack install-time' } else { '' }
    Write-Host "AAB: $aabOut ($mb MB$packNote; SDK alvo $TargetSdk)"
    # bundletool (optional: %APPDATA%\RealEngine\android\bundletool.jar or BUNDLETOOL): the bundle checked as Play would
    $btJar = if ($env:BUNDLETOOL) { $env:BUNDLETOOL } else { Join-Path $env:APPDATA 'RealEngine\android\bundletool.jar' }
    if (Test-Path $btJar) {
        & (Join-Path $env:JAVA_HOME 'bin\java.exe') -jar $btJar validate --bundle $aabOut | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'bundletool validate recusou o AAB' }
        Write-Host 'AAB conferido pelo bundletool'
    }
}
if ($Install -and $Format -ne 'aab') {
    $adb = Join-Path $sdk 'platform-tools\adb.exe'
    & $adb install -r $apkOut
    if ($LASTEXITCODE -ne 0) { throw 'adb install falhou (o aparelho ou o emulador está conectado?)' }
}
} finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage
}
