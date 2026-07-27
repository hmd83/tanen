# Build the standalone sensor bring-up app in test_apps\measure_loop.
#
#   .\build_test.ps1 [--pristine]
#
# Toolchain discovery + overrides: see scripts\ncs-env.ps1.

$repo = $PSScriptRoot
. (Join-Path $repo "scripts\ncs-env.ps1")

$pristine = if ($args -contains "--pristine") { "--pristine" } else { "" }
$boardRoot = (Join-Path $repo "third_party\seeed-xiao-nrf54lm20a").Replace('\', '/')

# --no-sysbuild is defensive: measure_loop has no sysbuild.conf of its own
# (unaffected by the app's sysbuild.conf, different source dir), but this keeps
# the test build a plain single-image flow if that ever changes.
west build -b xiao_nrf54lm20a/nrf54lm20a/cpuapp $pristine --no-sysbuild `
    -s (Join-Path $repo "test_apps\measure_loop") `
    -d (Join-Path $repo "build_test") -- `
    -DBOARD_ROOT="$boardRoot"
exit $LASTEXITCODE
