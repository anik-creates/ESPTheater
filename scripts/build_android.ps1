<#
.SYNOPSIS
    Builds the Android flasher (release/ESPTheater-Flasher.apk) with the Android SDK command-line tools.

.DESCRIPTION
    No Gradle and no third-party libraries: aapt2 + javac + d8 + zipalign + apksigner. The firmware images
    from release/firmware are packed into the APK, so the app is self-contained.

    Needs: Android SDK (build-tools 34+, platforms;android-34 or newer) and a JDK 17+ (Android Studio's
    bundled JBR is found automatically).

    Signing: uses android/signing/esptheater-release.jks (created on first run, password in
    android/signing/keystore.properties - both git-ignored; back them up, updates must be signed with the
    same key). In CI, set ESPTHEATER_KEYSTORE_BASE64 and ESPTHEATER_KEYSTORE_PASSWORD.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build_android.ps1
#>
param([string]$Sdk = "", [string]$Jdk = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$App = Join-Path $Root 'android\app\src\main'
$Out = Join-Path $Root 'build\android'
$Release = Join-Path $Root 'release'
$Apk = Join-Path $Release 'ESPTheater-Flasher.apk'
$MinSdk = 21
$TargetSdk = 34

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed with exit code $LASTEXITCODE" }
}

# ---- tools
if (-not $Sdk) { $Sdk = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, (Join-Path $env:LOCALAPPDATA 'Android\Sdk')) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1 }
if (-not $Sdk) { throw 'Android SDK not found. Install Android Studio or set ANDROID_HOME.' }
$BuildTools = Get-ChildItem (Join-Path $Sdk 'build-tools') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' -and [version]$_.Name -ge [version]'34.0.0' } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $BuildTools) { throw 'Android build-tools 34+ not found (sdkmanager "build-tools;35.0.0").' }
$Platform = Get-ChildItem (Join-Path $Sdk 'platforms') -Directory |
    Where-Object { $_.Name -match '^android-(\d+)$' -and [int]$Matches[1] -ge 34 } |
    Sort-Object { [int]($_.Name -replace 'android-', '') } | Select-Object -Last 1
if (-not $Platform) { throw 'Android platform 34+ not found (sdkmanager "platforms;android-35").' }
$AndroidJar = Join-Path $Platform.FullName 'android.jar'
if (-not $Jdk) { $Jdk = @($env:JAVA_HOME, "$env:ProgramFiles\Android\Android Studio\jbr") | Where-Object { $_ -and (Test-Path (Join-Path $_ 'bin\javac.exe')) } | Select-Object -First 1 }
if (-not $Jdk) { throw 'JDK not found. Install Android Studio or set JAVA_HOME.' }
$env:JAVA_HOME = $Jdk
$env:PATH = "$Jdk\bin;$env:PATH"
$bt = $BuildTools.FullName
Write-Host "SDK: $Sdk  build-tools $($BuildTools.Name)  $($Platform.Name)  JDK: $Jdk"

# ---- version (must match the firmware)
$Version = (Select-String -Path (Join-Path $Root 'firmware\EspUsbWifiAdapter\version.h') -Pattern 'FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$appVersion = (Select-String -Path (Join-Path $App 'java\com\aniksarker\esptheater\MainActivity.java') -Pattern 'VERSION = "([^"]+)"').Matches[0].Groups[1].Value
if ($appVersion -ne $Version) { throw "MainActivity.VERSION ($appVersion) must match version.h ($Version)" }
$v = [version]$Version
$VersionCode = $v.Major * 10000 + $v.Minor * 100 + $v.Build
Write-Host "ESPTheater Flasher v$Version (code $VersionCode)" -ForegroundColor Cyan

# ---- staging
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
$assets = Join-Path $Out 'assets\firmware'
$classes = Join-Path $Out 'classes'
$dex = Join-Path $Out 'dex'
New-Item -ItemType Directory -Force -Path $assets, $classes, $dex | Out-Null
foreach ($chip in 'esp32s3', 'esp32s2') {
    foreach ($kind in 'full', 'app') {
        $src = Join-Path $Release "firmware\esp-usb-wifi-$chip-$kind.bin"
        if (-not (Test-Path $src)) { throw "Missing $src - run scripts\build.ps1 first." }
        Copy-Item $src (Join-Path $assets "$chip-$kind.bin")
    }
}
Copy-Item (Join-Path $Release 'firmware\manifest.json') $assets

# ---- resources + manifest
$resZip = Join-Path $Out 'res.zip'
Invoke-Checked "$bt\aapt2.exe" @('compile', '--dir', (Join-Path $App 'res'), '-o', $resZip)
$unsigned = Join-Path $Out 'unsigned.apk'
Invoke-Checked "$bt\aapt2.exe" @('link', '-o', $unsigned, '-I', $AndroidJar, '--manifest', (Join-Path $App 'AndroidManifest.xml'),
    '--min-sdk-version', $MinSdk, '--target-sdk-version', $TargetSdk, '--version-code', $VersionCode, '--version-name', $Version,
    $resZip)

# ---- code
$sources = Get-ChildItem -Recurse (Join-Path $App 'java') -Filter *.java | ForEach-Object FullName
# java.* from the JDK's Java 8 API (android.jar's stubs can't compile lambdas), android.* from android.jar;
# d8 desugars the lambdas for Android
Invoke-Checked "$Jdk\bin\javac.exe" (@('--release', '8', '-Xlint:-options', '-encoding', 'UTF-8',
    '-classpath', $AndroidJar, '-d', $classes) + $sources)
$classFiles = Get-ChildItem -Recurse $classes -Filter *.class | ForEach-Object FullName
Invoke-Checked "$bt\d8.bat" (@('--release', '--min-api', $MinSdk, '--lib', $AndroidJar, '--output', $dex) + $classFiles)

Add-Type -AssemblyName System.IO.Compression.FileSystem
# dex and assets are added here rather than by aapt2, whose Windows build writes asset paths with backslashes
$zip = [IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $dex 'classes.dex'), 'classes.dex') | Out-Null
    foreach ($f in Get-ChildItem $assets -File) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, "assets/firmware/$($f.Name)") | Out-Null
    }
}
finally { $zip.Dispose() }

$aligned = Join-Path $Out 'aligned.apk'
Invoke-Checked "$bt\zipalign.exe" @('-p', '-f', '4', $unsigned, $aligned)

# ---- signing
$signDir = Join-Path $Root 'android\signing'
$keystore = Join-Path $signDir 'esptheater-release.jks'
$props = Join-Path $signDir 'keystore.properties'
New-Item -ItemType Directory -Force -Path $signDir | Out-Null
if ($env:ESPTHEATER_KEYSTORE_BASE64 -and $env:ESPTHEATER_KEYSTORE_PASSWORD) {
    $keystore = Join-Path $Out 'ci.jks'
    [IO.File]::WriteAllBytes($keystore, [Convert]::FromBase64String($env:ESPTHEATER_KEYSTORE_BASE64))
    $password = $env:ESPTHEATER_KEYSTORE_PASSWORD
} elseif (Test-Path $keystore) {
    $password = (Select-String -Path $props -Pattern '^password=(.+)$').Matches[0].Groups[1].Value
} else {
    Write-Host 'Creating a signing key (android/signing/) - back it up: app updates must use the same key.' -ForegroundColor Yellow
    $password = -join ((48..57) + (65..90) + (97..122) | Get-Random -Count 24 | ForEach-Object { [char]$_ })
    Invoke-Checked "$Jdk\bin\keytool.exe" @('-genkeypair', '-keystore', $keystore, '-alias', 'esptheater', '-keyalg', 'RSA',
        '-keysize', '2048', '-validity', '10000', '-storepass', $password, '-keypass', $password,
        '-dname', 'CN=Anik Sarker, O=ESPTheater')
    Set-Content -Encoding ASCII $props "password=$password"
}
Invoke-Checked "$bt\apksigner.bat" @('sign', '--ks', $keystore, '--ks-key-alias', 'esptheater', '--ks-pass', "pass:$password",
    '--key-pass', "pass:$password", '--out', $Apk, $aligned)
Invoke-Checked "$bt\apksigner.bat" @('verify', $Apk)
Remove-Item "$Apk.idsig" -ErrorAction SilentlyContinue
Write-Host ("Built {0} ({1:N0} bytes)" -f $Apk.Substring($Root.Length + 1), (Get-Item $Apk).Length) -ForegroundColor Green
