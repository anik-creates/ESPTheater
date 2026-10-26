<#
.SYNOPSIS
    Builds the firmware for every supported chip and the Windows flasher.

.DESCRIPTION
    Outputs (committed to the repo so a plain "Download ZIP" from GitHub is ready to use):
      release/ESP-USB-WiFi-Flasher.exe              - flasher with esptool and all firmware embedded
      release/firmware/esp-usb-wifi-<chip>-full.bin - complete image, flash at 0x0
      release/firmware/esp-usb-wifi-<chip>-app.bin  - application only, flash at 0x10000 (keeps settings)
      release/firmware/manifest.json, SHA256SUMS.txt
    plus build/dist/ESP-USB-WiFi-Adapter-v<version>-windows.zip for GitHub releases.

    Needs arduino-cli with the esp32 core 3.3.2 (the Arduino IDE 2 bundles arduino-cli) and the
    .NET Framework 4 C# compiler that ships with Windows.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
#>
param(
    [string]$ArduinoCli = "",
    [switch]$SkipFlasher,
    [switch]$InstallCore
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Sketch = Join-Path $Root 'firmware\EspUsbWifiAdapter'
$Release = Join-Path $Root 'release'
$FwOut = Join-Path $Release 'firmware'
$Build = Join-Path $Root 'build'
$CoreVersion = '3.3.2'
$CoreIndexUrl = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'

$Version = (Select-String -Path (Join-Path $Sketch 'version.h') -Pattern 'FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
Write-Host "ESP USB Wi-Fi Adapter v$Version" -ForegroundColor Cyan

$Targets = @(
    @{ Chip = 'esp32s3'; Name = 'ESP32-S3'
       Fqbn = 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,PartitionScheme=huge_app,FlashMode=dio,FlashSize=4M,PSRAM=disabled,DebugLevel=info' },
    @{ Chip = 'esp32s2'; Name = 'ESP32-S2'
       Fqbn = 'esp32:esp32:esp32s2:CDCOnBoot=default,PartitionScheme=huge_app,FlashMode=dio,FlashSize=4M,PSRAM=disabled,DebugLevel=info' }
)

function Find-ArduinoCli {
    if ($ArduinoCli) { return $ArduinoCli }
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $ide = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
    if (Test-Path $ide) { return $ide }
    throw 'arduino-cli not found. Install the Arduino IDE 2 or arduino-cli, or pass -ArduinoCli <path>.'
}

function Invoke-Checked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$([IO.Path]::GetFileName($exe)) failed with exit code $LASTEXITCODE" }
}

# Copies a merged image without the 0xFF padding Arduino adds up to the full flash size
function Copy-Trimmed([string]$src, [string]$dst) {
    $bytes = [IO.File]::ReadAllBytes($src)
    $end = $bytes.Length
    while ($end -gt 0 -and $bytes[$end - 1] -eq 0xFF) { $end-- }
    $end = [Math]::Ceiling($end / 4096) * 4096
    $out = New-Object byte[] $end
    [Array]::Copy($bytes, $out, [Math]::Min($end, $bytes.Length))
    for ($i = $bytes.Length; $i -lt $end; $i++) { $out[$i] = 0xFF }
    [IO.File]::WriteAllBytes($dst, $out)
}

$cli = Find-ArduinoCli
Write-Host "arduino-cli: $cli"

if ($InstallCore) {
    Invoke-Checked $cli @('core', 'update-index', '--additional-urls', $CoreIndexUrl)
    Invoke-Checked $cli @('core', 'install', "esp32:esp32@$CoreVersion", '--additional-urls', $CoreIndexUrl)
}
$coreList = & $cli core list
if (-not ($coreList | Select-String -Pattern "esp32:esp32\s+$([regex]::Escape($CoreVersion))")) {
    throw "esp32 core $CoreVersion is required (the USB fixes depend on it). Run with -InstallCore, or: arduino-cli core install esp32:esp32@$CoreVersion"
}

New-Item -ItemType Directory -Force -Path $FwOut, $Build | Out-Null
$manifest = [ordered]@{ name = 'ESP USB Wi-Fi Adapter'; version = $Version; core = "arduino-esp32 $CoreVersion"; chips = @() }

foreach ($t in $Targets) {
    Write-Host "`n== Building $($t.Name)" -ForegroundColor Cyan
    $out = Join-Path $Build $t.Chip
    Invoke-Checked $cli @('compile', '--fqbn', $t.Fqbn, '--output-dir', $out, '--warnings', 'default', $Sketch)
    $full = Join-Path $FwOut "esp-usb-wifi-$($t.Chip)-full.bin"
    $app = Join-Path $FwOut "esp-usb-wifi-$($t.Chip)-app.bin"
    Copy-Trimmed (Join-Path $out 'EspUsbWifiAdapter.ino.merged.bin') $full
    Copy-Item (Join-Path $out 'EspUsbWifiAdapter.ino.bin') $app -Force
    $manifest.chips += [ordered]@{
        chip = $t.Chip; name = $t.Name
        full = [ordered]@{ file = [IO.Path]::GetFileName($full); offset = '0x0'; sha256 = (Get-FileHash $full -Algorithm SHA256).Hash.ToLower() }
        app  = [ordered]@{ file = [IO.Path]::GetFileName($app); offset = '0x10000'; sha256 = (Get-FileHash $app -Algorithm SHA256).Hash.ToLower() }
    }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $FwOut 'manifest.json')

if (-not $SkipFlasher) {
    Write-Host "`n== Building the flasher" -ForegroundColor Cyan
    $esptool = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\tools\esptool_py\*\esptool.exe') |
               Sort-Object { [version]($_.Directory.Name -replace '[^\d\.].*$', '') } | Select-Object -Last 1
    if (-not $esptool) { throw 'esptool.exe not found in the Arduino esp32 tools.' }
    Write-Host "esptool: $($esptool.FullName)"
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    if (-not (Test-Path $csc)) { $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe' }
    $src = Join-Path $Root 'flasher\Flasher.cs'
    (Get-Content $src -Raw) -match 'public const string Version = "([^"]+)"' | Out-Null
    if ($Matches[1] -ne $Version) { throw "flasher\Flasher.cs BuildInfo.Version ($($Matches[1])) must match version.h ($Version)" }
    $exe = Join-Path $Release 'ESP-USB-WiFi-Flasher.exe'
    $cscArgs = @('/nologo', '/target:winexe', '/optimize+', "/out:$exe", "/win32icon:$(Join-Path $Root 'flasher\app.ico')",
                 '/r:System.dll', '/r:System.Core.dll', '/r:System.Drawing.dll', '/r:System.Windows.Forms.dll', '/r:System.Management.dll',
                 "/resource:$($esptool.FullName),esptool.exe")
    foreach ($t in $Targets) {
        $cscArgs += "/resource:$(Join-Path $FwOut "esp-usb-wifi-$($t.Chip)-full.bin"),$($t.Chip)-full.bin"
        $cscArgs += "/resource:$(Join-Path $FwOut "esp-usb-wifi-$($t.Chip)-app.bin"),$($t.Chip)-app.bin"
    }
    $cscArgs += $src
    Invoke-Checked $csc $cscArgs
}

Push-Location $Release
try {
    Get-ChildItem -Recurse -File -Include *.exe, *.bin, manifest.json |
        ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), (Resolve-Path -Relative $_.FullName).TrimStart('.', '\').Replace('\', '/') } |
        Set-Content -Encoding ASCII (Join-Path $Release 'SHA256SUMS.txt')
} finally { Pop-Location }

$dist = Join-Path $Build 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist "ESP-USB-WiFi-Adapter-v$Version-windows.zip"
if (Test-Path $zip) { Remove-Item $zip }
$staging = Join-Path $Build "zip\ESP-USB-WiFi-Adapter-v$Version"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force -Path $staging | Out-Null
Copy-Item -Recurse (Join-Path $Release '*') $staging
Copy-Item (Join-Path $Root 'README.md'), (Join-Path $Root 'LICENSE'), (Join-Path $Root 'THIRD_PARTY_NOTICES.md') $staging
Compress-Archive -Path $staging -DestinationPath $zip
Write-Host "`nDone:" -ForegroundColor Green
Get-ChildItem -Recurse $Release -File | ForEach-Object { "  {0,-60} {1,10:N0} bytes" -f $_.FullName.Substring($Root.Length + 1), $_.Length }
"  {0,-60} {1,10:N0} bytes" -f $zip.Substring($Root.Length + 1), (Get-Item $zip).Length
