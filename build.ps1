# TanenBase firmware build (Windows / PowerShell).
#
#   .\build.ps1 [debug|prod|<file.conf>] [--pristine] [--sysbuild]
#
# Toolchain discovery + overrides: see scripts\ncs-env.ps1.

$repo  = $PSScriptRoot
$build = Join-Path $repo "build"

. (Join-Path $repo "scripts\ncs-env.ps1")

# --- Build mode -------------------------------------------------------------
# First non-flag arg selects the build type (default: debug)
$mode = if ($args[0] -and $args[0] -ne "--pristine" -and $args[0] -ne "--sysbuild") { $args[0] } else { "debug" }
$overlay = switch ($mode) {
    "debug"      { "debug.conf" }
    "production" { "prj_production.conf" }
    "prod"       { "prj_production.conf" }
    default      { $mode }  # allow passing an explicit .conf filename
}
$pristine = if ($args -contains "--pristine") { "--pristine" } else { "" }

# Switching mode in-place is unsafe: west caches SNIPPET (debug's cdc-acm-console
# leaks into prod) and CMakeCache keeps nrf_security's CONFIG_* STRING entries,
# which kconfig.cmake re-emits unquoted -> "malformed string literal" -> abort.
# Force pristine whenever the mode differs from the last build in this dir.
$modeStamp = Join-Path $build ".build_mode"
if (-not $pristine -and (Test-Path $modeStamp) -and ((Get-Content $modeStamp -Raw).Trim() -ne $mode)) {
    Write-Host "Mode changed -> forcing --pristine" -ForegroundColor Yellow
    $pristine = "--pristine"
}

# sysbuild.conf in the app dir makes west auto-select sysbuild (MCUboot).
# Default to --no-sysbuild so the everyday build stays the plain single-image
# flow flash_*.ps1 expects (output build\zephyr\zephyr.hex, not merged.hex).
# Pass --sysbuild to opt into the OTA/MCUboot image.
# NOTE: --sysbuild is UNVERIFIED on LM20A / NCS v3.4.0. On the earlier L15 /
# v3.2.4 port it failed in MCUboot's flash_map_extended.c (undeclared
# FLASH_DEVICE_BASE / FLASH_DEVICE_ID).
$sysbuildFlag = if ($args -contains "--sysbuild") { "" } else { "--no-sysbuild" }

# Board def is out-of-tree: the vendored Seeed board has no zephyr/module.yml,
# so west can't auto-discover it and BOARD_ROOT must be passed explicitly.
$boardRoot = (Join-Path $repo "third_party\seeed-xiao-nrf54lm20a").Replace('\', '/')

# prj_credentials.conf holds the real LoRaWAN AppKey and is gitignored. Fall
# back to the committed example so a fresh clone still builds — with placeholder
# keys, which will not join a real network.
$creds = if (Test-Path (Join-Path $repo "prj_credentials.conf")) {
    "prj_credentials.conf"
} else {
    Write-Host "prj_credentials.conf missing -> using prj_credentials.conf.example (placeholder keys, will not join TTN)" -ForegroundColor Yellow
    "prj_credentials.conf.example"
}

# Console/logs go over uart20 (P1.11 TX / P1.10 RX) -> on-board CMSIS-DAP VCOM
# (the same 2886:0068 composite openocd flashes through) -> COM port @115200.
# Do NOT use -S cdc-acm-console: that repoints zephyr,console at the nRF's own
# USBHS CDC (VID 2fe3), which never enumerates on this board -- silent console.
# P1.10/P1.11 are not on the XIAO header (see seeed_xiao_connector.dtsi), they
# are dedicated to the debug chip, so uart20 costs no usable pins.

Write-Host "NCS $NcsVersion | toolchain $(Split-Path $NcsToolchain -Leaf)" -ForegroundColor DarkGray
Write-Host "Building '$mode' (overlay: $overlay)" -ForegroundColor Cyan

west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp -d $build $pristine $sysbuildFlag -s $repo -- `
    -DBOARD_ROOT="$boardRoot" `
    -DOVERLAY_CONFIG="$creds;$overlay"

if ($LASTEXITCODE -eq 0) { Set-Content -Path $modeStamp -Value $mode }
exit $LASTEXITCODE
