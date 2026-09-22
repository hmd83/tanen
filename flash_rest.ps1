# Full factory reset — CTRL-AP mass erase (wipes ZMS: DevNonce, session,
# credentials, scale calibration) then reflash. Use flash_update.ps1 to keep data.
#
#   .\flash_rest.ps1 [build|build_test]
#
# NOTE: erasing the DevNonce counter means the next OTAA join replays nonces the
# network has already seen. TTN rejects those — reset the device's join nonce
# counters in the TTN console (or re-register the device) after a factory reset.
#
# OpenOCD path: pyocd's erase works but its programming algorithm hard-faults
# on nRF54LM20A (IPSR=3), which leaves the chip blank. See flash_update.ps1.
param([string]$BuildDir = "build")

$repo = $PSScriptRoot
$hex  = (Join-Path $repo "$BuildDir\zephyr\zephyr.hex").Replace('\', '/')
$cfg  = Join-Path $repo "third_party\seeed-xiao-nrf54lm20a\boards\arm\xiao_nrf54lm20a\support\openocd.cfg"

if (-not (Test-Path $hex)) { Write-Error "No hex at $hex — build first."; exit 1 }
Write-Host "FACTORY RESET (mass erase) + flash $hex" -ForegroundColor Yellow

# Prefer the real binary over chocolatey's bin\ shim: Windows Application Control
# blocks the shim ("An Application Control policy has blocked this file").
$openocd = 'C:\ProgramData\chocolatey\lib\openocd\tools\install\bin\openocd.exe'
if (-not (Test-Path $openocd)) { $openocd = 'openocd' }

# See flash_update.ps1 for why the WEN clear and the verify are here.
. (Join-Path $PSScriptRoot "scripts/flash-lib.ps1")

# load_image drops the final partial 16-byte RRAM line; rewrite those words
# explicitly. Empty when the image happens to end on a line boundary.
$tail = @(Get-HexTailWrites $hex)
if ($tail.Count) { Write-Host ("Tail repair: {0} word(s) after load" -f $tail.Count) -ForegroundColor DarkGray }

$cmd = "init; nrf54l_mass_erase; halt; mww 0x5004e500 0x101; load_image $hex"
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
