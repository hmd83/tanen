# Shared flashing helper: recover the image tail that openocd's load_image drops.
#
# On nRF54LM20A, load_image leaves the final PARTIAL 16-byte RRAM line in the
# controller's write buffer and it is never committed, so the last 4/8/12 bytes
# of the image read back as 0xff. Clearing WEN afterwards does NOT flush it
# (tested 2026-09-22 -- an apparent pass was residue from bytes patched by hand
# in a previous session; RRAM is non-volatile and load_image simply skips them,
# so the stale-but-correct values survived and faked a fix. A mass erase exposed
# the truth).
#
# What does work is writing those words explicitly with mww. This builds those
# commands from the hex, so the fix needs no knowledge of the image layout and
# keeps working as the image grows.
#
# Why it matters: the dropped tail lands on whatever the linker put last, and
# .last_section sits at the very end with net_buf_pool_area against it. One
# dropped pool pointer read 0xFFFFFFFF, and 0xFFFFFFFF+0xA wrapped to address 9
# -- a BUS FAULT deep inside bt_enable that looked like a Bluetooth bug.

function Get-HexTailWrites {
    param([Parameter(Mandatory)][string]$HexPath)

    $bytes = @{}
    $upper = 0
    foreach ($line in [System.IO.File]::ReadLines($HexPath)) {
        if (-not $line.StartsWith(':')) { continue }
        $len  = [Convert]::ToInt32($line.Substring(1,2),16)
        $adr  = [Convert]::ToInt32($line.Substring(3,4),16)
        $type = [Convert]::ToInt32($line.Substring(7,2),16)
        switch ($type) {
            4 { $upper = [Convert]::ToInt32($line.Substring(9,4),16) }
            2 { $upper = [Convert]::ToInt32($line.Substring(9,4),16) -shr 12 }
            0 {
                $base = ($upper -shl 16) -bor $adr
                for ($i = 0; $i -lt $len; $i++) {
                    $bytes[$base + $i] = [Convert]::ToByte($line.Substring(9 + $i*2, 2), 16)
                }
            }
        }
    }
    if ($bytes.Count -eq 0) { throw "no data records in $HexPath" }

    # [int] casts matter: Measure-Object returns a Double, and a Double key never
    # matches the Int32 keys in the table -- every lookup would silently miss and
    # the tail would be rebuilt as 0xffffffff, i.e. exactly the corruption we are
    # trying to repair.
    $end = [int](($bytes.Keys | Measure-Object -Maximum).Maximum) + 1
    $lineStart = [int]($end -band (-bnot 0xF))
    if ($lineStart -eq $end) { return @() }   # ends on a line boundary: nothing dropped

    # Write the FULL 16-byte line, not just the missing bytes. The RRAM write
    # buffer only commits once a whole line has been written, so a partial mww
    # just joins the data already stranded in it and nothing reaches the array
    # (observed: a 2-word tail repair changed nothing). The words past the image
    # end are written as 0xffffffff, which is the erased value, so padding into
    # the unused space below the storage partition costs nothing.
    $cmds = @()
    for ([int]$a = $lineStart; $a -lt ($lineStart + 16); $a += 4) {
        $w = 0
        for ([int]$b = 3; $b -ge 0; $b--) {
            $k = [int]($a + $b)
            $v = if ($bytes.ContainsKey($k)) { $bytes[$k] } else { 0xFF }
            $w = ($w -shl 8) -bor $v
        }
        $cmds += ("mww 0x{0:x8} 0x{1:x8}" -f $a, $w)
    }
    return $cmds
}
