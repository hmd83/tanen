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

# TCL needs forward slashes in the path; halt first in case the target is
# running or locked up (blank flash → double fault, cfg auto-recovers).
openocd -f $cfg -c "init; halt; nrf54lm20a-load $hex; reset run; shutdown"
exit $LASTEXITCODE
