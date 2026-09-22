# Flash firmware only — preserves ZMS config/calibration data in RRAM.
# Use flash_rest.ps1 for a full factory reset (mass erase).
#
#   .\flash_update.ps1 [build|build_test]
#
# Uses OpenOCD with Seeed's board config, which writes RRAM directly over SWD
# (`mww 0x5004e500 0x101` = WEN + write-buffer size, then load_image).
# pyocd is NOT usable on nRF54LM20A: its builtin flash algorithm hard-faults
# during programming ("target was not halted as expected ... IPSR=3"), though
# its erase works. Erase-then-fail leaves the chip blank, so don't retry it.
#
# build.ps1 uses --no-sysbuild (plain single image) — output is zephyr/zephyr.hex,
# not merged.hex (merged.hex only appears if --sysbuild/MCUboot is ever enabled).
param([string]$BuildDir = "build")

$repo = $PSScriptRoot
$hex  = (Join-Path $repo "$BuildDir\zephyr\zephyr.hex").Replace('\', '/')
$cfg  = Join-Path $repo "third_party\seeed-xiao-nrf54lm20a\boards\arm\xiao_nrf54lm20a\support\openocd.cfg"

if (-not (Test-Path $hex)) { Write-Error "No hex at $hex — build first."; exit 1 }
Write-Host "Flashing $hex" -ForegroundColor Cyan

# Prefer the real binary over chocolatey's bin\ shim: Windows Application Control
# blocks the shim ("An Application Control policy has blocked this file").
$openocd = 'C:\ProgramData\chocolatey\lib\openocd\tools\install\bin\openocd.exe'
if (-not (Test-Path $openocd)) { $openocd = 'openocd' }

# TCL needs forward slashes in the path; halt first in case the target is
# running or locked up (blank flash → double fault, cfg auto-recovers).

# RRAM WRITE-BUFFER FLUSH + VERIFY (2026-09-22) -- do not remove.
#
# openocd's load_image leaves the final PARTIAL 16-byte RRAM line sitting in the
# controller's write buffer, so the last few bytes of the image are never
# committed. Clearing WEN (CONFIG=0) after the load flushes it.
#
# This cost a full day. Symptoms it produced, none of which look like a flashing
# problem: boot loops, HardFault at z_arm_exc_spurious, and a BUS FAULT at
# BFAR 0x9 inside net_buf_alloc_len during bt_enable -- because .last_section
# sits at the very end of the image and net_buf_pool_area ends right against it,
# so a dropped tail truncated a net_buf pool's RAM pointer to 0xFFFFFFFF
# (0xFFFFFFFF + 0xA wraps to 0x00000009 = the faulting address).
#
# Whether it bites depends on image SIZE: if the image happens to end on a
# 16-byte boundary the dropped tail is empty, and if only .last_section falls in
# it the marker is inert. Adding ~900 bytes of app code moved live pool data
# into the tail and turned a silent defect into a hard fault. So a build that
# works proves nothing about the next one -- hence verify_image below, which
# fails the flash loudly instead of handing back a subtly broken device.

# The verify runs after a reset+settle: straight after a load the core can still
# be in the halted-from-lockup state, where verify_image silently does nothing
# and would report a false pass.
. (Join-Path $PSScriptRoot "scripts/flash-lib.ps1")

# load_image drops the final partial 16-byte RRAM line; rewrite those words
# explicitly. Empty when the image happens to end on a line boundary.
$tail = @(Get-HexTailWrites $hex)
if ($tail.Count) { Write-Host ("Tail repair: {0} word(s) after load" -f $tail.Count) -ForegroundColor DarkGray }

$cmd = "init; halt; mww 0x5004e500 0x101; load_image $hex"
foreach ($w in $tail) { $cmd += "; $w" }
$cmd += "; mww 0x5004e500 0x0; reset run; shutdown"

& $openocd -f $cfg -c $cmd 2>&1 | ForEach-Object { Write-Host $_ }

# Verify in a SEPARATE openocd session, with the device running normally.
# verify_image uploads a CRC routine and executes it on the target, so it needs
# a stable halted core: chaining it after the load in one session catches the
# core still in its post-lockup state and it silently does nothing (a false
# pass), and chaining it after "reset run" in the same session races the
# firmware's own resets. A fresh session that halts a running device works.
Start-Sleep -Milliseconds 800
$vout = & $openocd -f $cfg -c "init; halt; verify_image $hex; reset run; shutdown" 2>&1
$halted  = [bool]($vout | Select-String -Pattern 'halted due to debug-request' -Quiet)
$baddiff = [bool]($vout | Select-String -Pattern '^diff \d+ address' -Quiet)

if ($baddiff) {
    $vout | Select-String -Pattern '^diff \d+ address' | ForEach-Object { Write-Host $_ }
    Write-Error "FLASH INCOMPLETE - the bytes above never committed. The device will fault in unrelated places (BLE, net_buf, boot loops). Do not use this device."
    exit 1
}
if (-not $halted) {
    Write-Warning "Could not halt the target, so the flash is UNVERIFIED. Re-run while the device is awake (press the button first) to confirm."
} else {
    Write-Host "Flash verified: full image committed." -ForegroundColor Green
}
exit 0
