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

openocd -f $cfg -c "init; nrf54l_mass_erase; halt; nrf54lm20a-load $hex; reset run; shutdown"
exit $LASTEXITCODE
