# Dot-source this to put the nRF Connect SDK toolchain on PATH and set
# ZEPHYR_BASE, without depending on any one machine's install layout.
#
# Overrides (all optional):
#   $env:NCS_ROOT       SDK install root                    (default: C:\ncs)
#   $env:NCS_VERSION    version dir under NCS_ROOT          (default: newest vX.Y.Z)
#   $env:NCS_TOOLCHAIN  full path to a toolchains\<hash> dir (default: newest)

$ncsRoot = if ($env:NCS_ROOT) { $env:NCS_ROOT } else { "C:\ncs" }
if (-not (Test-Path $ncsRoot)) {
    throw "nRF Connect SDK not found at '$ncsRoot'. Install it or set `$env:NCS_ROOT."
}

$script:NcsVersion = if ($env:NCS_VERSION) { $env:NCS_VERSION } else {
    (Get-ChildItem $ncsRoot -Directory |
        Where-Object { $_.Name -match '^v\d+\.\d+\.\d+' } |
        Sort-Object Name -Descending | Select-Object -First 1).Name
}

$zephyrBase = Join-Path $ncsRoot "$script:NcsVersion\zephyr"
if (-not (Test-Path $zephyrBase)) {
    $found = (Get-ChildItem $ncsRoot -Directory -Name) -join ', '
    throw "No Zephyr tree at '$zephyrBase'. Set `$env:NCS_VERSION (found under ${ncsRoot}: $found)."
}

$script:NcsToolchain = if ($env:NCS_TOOLCHAIN) { $env:NCS_TOOLCHAIN } else {
    (Get-ChildItem (Join-Path $ncsRoot "toolchains") -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
if (-not $script:NcsToolchain -or -not (Test-Path $script:NcsToolchain)) {
    throw "No NCS toolchain under '$ncsRoot\toolchains'. Set `$env:NCS_TOOLCHAIN."
}

# west/cmake must not pick up a system Python or its site-packages
$env:PYTHONPATH = ""
$env:PYTHONHOME = ""
$env:PATH = "$script:NcsToolchain\opt\bin;$script:NcsToolchain\opt\bin\Scripts;" + $env:PATH
$env:ZEPHYR_BASE = $zephyrBase
