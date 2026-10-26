param(
    [string]$Port = "",
    [switch]$BuildOnly,
    [switch]$DebugFirmware
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$HardwareDir = Join-Path $RepoRoot "hardware"
$SketchDir = Join-Path $HardwareDir "hid_bridge"
$BuildDir = Join-Path $HardwareDir "build-windows"
$ArduinoCli = "D:\Apps\ArduinoCLI\arduino-cli.exe"
$ArduinoConfig = "D:\Arduino\config\arduino-cli.yaml"

if (-not (Test-Path $ArduinoCli)) {
    throw "Arduino CLI not found at $ArduinoCli"
}

$Fqbn = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PSRAM=opi,PartitionScheme=default_8MB"
$NimbleConfig = (Join-Path $SketchDir "nimble_cfg.h").Replace("\", "/")
$ExtraFlags = "-include $NimbleConfig"
if ($DebugFirmware) { $ExtraFlags += " -DDEBUG_SERIAL=1" }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Write-Host "Building ESP32-S3 firmware..."
& $ArduinoCli compile `
    --config-file $ArduinoConfig `
    --fqbn $Fqbn `
    --build-property "compiler.cpp.extra_flags=$ExtraFlags" `
    --build-property "compiler.c.extra_flags=$ExtraFlags" `
    --output-dir $BuildDir `
    $SketchDir
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed." }

if ($BuildOnly) {
    Write-Host "Build completed: $BuildDir"
    exit 0
}

if (-not $Port) {
    $Port = Get-CimInstance Win32_SerialPort |
        Where-Object { $_.PNPDeviceID -match "VID_303A" } |
        Select-Object -First 1 -ExpandProperty DeviceID
}
if (-not $Port) {
    throw "ESP32-S3 serial port not found. Hold BOOT, tap RESET, release BOOT, then retry with -Port COMx."
}

Write-Host "Uploading firmware through $Port..."
& $ArduinoCli upload `
    --config-file $ArduinoConfig `
    --fqbn $Fqbn `
    --port $Port `
    --input-dir $BuildDir `
    $SketchDir
if ($LASTEXITCODE -ne 0) { throw "Firmware upload failed." }

Write-Host "Firmware uploaded successfully."
