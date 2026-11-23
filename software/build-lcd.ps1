[CmdletBinding()]
param(
    [string]$SdkRoot = $(if ($env:W600_SDK) { $env:W600_SDK } else { "F:/projects/w600-sdk" }),
    [ValidateSet("normal", "board", "flashonly", "badapple")]
    [string]$Variant = "normal",
    [string]$Toolchain = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$Source = Join-Path $PSScriptRoot "lcd"
$Dest = Join-Path $SdkRoot "example/lcd"
$Makefile = Join-Path $SdkRoot "example/Makefile.mk"

if (-not (Test-Path $Makefile)) {
    throw "W600 SDK not found at '$SdkRoot' (missing example/Makefile.mk). Pass -SdkRoot or set W600_SDK."
}
if (-not (Test-Path $Source)) {
    throw "LCD source not found at '$Source'."
}

if (-not $Toolchain) {
    $Toolchain = Join-Path $env:LOCALAPPDATA "Arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4/bin"
}
$Gcc = Join-Path $Toolchain "arm-none-eabi-gcc.exe"
if (-not (Test-Path $Gcc)) {
    throw "ARM GCC not found at '$Gcc'. Pass -Toolchain <bin-dir>."
}

Remove-Item $Dest -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item $Source $Dest -Recurse -Force
Remove-Item (Join-Path $Dest "user/.output") -Recurse -Force -ErrorAction SilentlyContinue

$ExtraFlags = switch ($Variant) {
    "normal"    { "" }
    "board"     { "-DBOARD_CHECK -DSD_DIAG" }
    "flashonly" { "-DFLASH_ONLY" }
    "badapple"  { "-DBAD_APPLE" }
}

$MakeArgs = @(
    "-f", "Makefile.mk",
    "APP_BIN_NAME=lcd",
    "TARGET=lcd",
    "COMPILE=gcc",
    "FLASH_SIZE=1M",
    "CC=$Toolchain/arm-none-eabi-gcc.exe",
    "AR=$Toolchain/arm-none-eabi-ar.exe",
    "ASM=$Toolchain/arm-none-eabi-gcc.exe",
    "OBJCOPY=$Toolchain/arm-none-eabi-objcopy.exe",
    "LINK=$Toolchain/arm-none-eabi-ld.exe",
    "OBJDUMP=$Toolchain/arm-none-eabi-objdump.exe"
)
if ($ExtraFlags) {
    $MakeArgs += "EXTRA_CCFLAGS=$ExtraFlags"
}

Push-Location (Join-Path $SdkRoot "example")
try {
    & make -f Makefile.mk clean APP_BIN_NAME=lcd
    if ($LASTEXITCODE -ne 0) {
        throw "make clean failed with exit code $LASTEXITCODE"
    }

    & make @MakeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "make failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$Output = Join-Path $SdkRoot "bin/lcd/lcd.fls"
if (-not (Test-Path $Output)) {
    throw "Build finished but '$Output' was not produced."
}

$Prebuilt = Join-Path $RepoRoot "software/prebuilt/$Variant"
New-Item -ItemType Directory -Force -Path $Prebuilt | Out-Null
$Target = Join-Path $Prebuilt "lcd.fls"
Copy-Item $Output $Target -Force

Write-Host ""
Write-Host "Variant : $Variant"
Write-Host "Image   : $Target"
$Md5 = [System.Security.Cryptography.MD5]::Create()
$Hash = [BitConverter]::ToString($Md5.ComputeHash([IO.File]::ReadAllBytes($Target))).Replace("-", "").ToLowerInvariant()
$Md5.Dispose()
Write-Host "MD5     : $Hash"
