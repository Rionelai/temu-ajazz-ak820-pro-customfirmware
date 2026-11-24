# Update U8 (the screen chip) over USB - no Pico. Needs the u8link firmware on the keyboard:
#   .\tools\update.ps1 -Bin .\firmware\out\u8link.bin      (hold Fn+Esc 3 s)
#   .\tools\u8update.ps1 [-Bin .\firmware\out\u8app.bin]
#   .\tools\update.ps1 -Bin .\firmware\out\keyboard.bin    (back to normal)
# Talks to U8's bootloader (firmware/u8/u8boot.c) through u8link (firmware/u8link/u8link.c).
param([string]$Bin = "$PSScriptRoot\..\firmware\out\u8app.bin", [string]$Port = '', [switch]$PingOnly)
$ErrorActionPreference = 'Stop'
if (-not $Port) {
    $d = Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match 'VID_1209&PID_0001' -and $_.Name -match '\((COM\d+)\)' } | Select-Object -First 1
    if (-not $d) { Write-Host 'Keyboard serial port not found - is the u8link firmware installed?'; exit 1 }
    $Port = [regex]::Match($d.Name, 'COM\d+').Value
}
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 3000
$sp.Open(); Start-Sleep -Milliseconds 200; $sp.DiscardInBuffer()

function ReadN([int]$n) { $b = New-Object byte[] $n; $o = 0; while ($o -lt $n) { $o += $sp.Read($b, $o, $n - $o) }; ,$b }
# send bytes on the wire, get up to $rn answer bytes
# 'X' = bootloader speed (115200, open-drain), 'Y' = screen firmware v6+ (460800, push-pull)
$script:fast = $false
function Xfer([byte[]]$tx, [int]$rn, [int]$tmo = 50) {
    $h = [byte[]]($(if ($script:fast) { 0x59 } else { 0x58 }), ($tx.Length -band 0xFF), ($tx.Length -shr 8), $rn, $tmo)
    $sp.Write($h, 0, 5); $sp.Write($tx, 0, $tx.Length)
    $x = ReadN 2
    if ($x[0] -ne 0x78) { throw "bridge answered $($x[0])" }
    if ($x[1] -eq 0) { return ,([byte[]]@()) }
    ,(ReadN $x[1])
}
function Frame([char]$cmd, [int]$addr, [byte[]]$data) {
    if (-not $data) { $data = [byte[]]@() }
    $f = [byte[]](0x5A, [byte]$cmd, ($addr -band 0xFF), (($addr -shr 8) -band 0xFF), ($data.Length -band 0xFF), ($data.Length -shr 8)) + $data
    $s = 0; for ($i = 1; $i -lt $f.Length; $i++) { $s = ($s + $f[$i]) -band 0xFF }
    [byte[]]($f + [byte]$s)
}
# screen firmware v2+ only listens between drawing steps: knock with 7F until it answers 79
function Wake { for ($i = 0; $i -lt 60; $i++) { $r = Xfer ([byte[]]@(0x7F)) 1 2; if ($r.Length -eq 1 -and $r[0] -eq 0x79) { return $true } }; $false }
function Ping1 { $null = Wake; $r = Xfer (Frame 'P' 0) 3 50; if ($r.Length -eq 3 -and $r[0] -eq 0x79) { "$([char]$r[1])$($r[2])" } else { '' } }
function Ping { $script:fast = $true; $w = Ping1; if ($w) { return $w }; $script:fast = $false; Ping1 }   # app v6 fast, else slow

$sp.Write('?'); $id = [System.Text.Encoding]::ASCII.GetString((ReadN 4))
if ($id -ne 'u8L1') { throw "not the u8link firmware (got '$id')" }
$who = Ping
Write-Host "U8 says: $(if ($who) { $who } else { 'nothing' })  (B = bootloader, A = screen firmware)"
if ($PingOnly) { $sp.Close(); exit 0 }
if ($who -like 'A*') {
    Write-Host 'restarting U8 into its bootloader ...'
    $null = Wake; $null = Xfer (Frame 'B' 0) 1 50; $script:fast = $false; Start-Sleep -Milliseconds 300; $who = Ping
}
if ($who -notlike 'B*') {
    Write-Host 'no answer - trying to catch the bootloader after a reset ...'
    $sp.Write('Z'); $z = ReadN 2; Start-Sleep -Milliseconds 50; $who = Ping
    if ($who -notlike 'B*') { Write-Host 'U8 bootloader not reachable. Unplug + replug the keyboard and run this again.'; $sp.Close(); exit 1 }
}
$img = [System.IO.File]::ReadAllBytes((Resolve-Path $Bin))
if ($img.Length -gt 0xF000) { throw 'image too big' }
$pages = [Math]::Ceiling($img.Length / 256)
Write-Host "writing $($img.Length) bytes ($pages pages) to U8 at 0x800 ..."
for ($p = 0; $p -lt $pages; $p++) {
    $d = New-Object byte[] 256
    for ($i = 0; $i -lt 256; $i++) { $j = $p * 256 + $i; $d[$i] = if ($j -lt $img.Length) { $img[$j] } else { 0xFF } }
    $ok = $false
    for ($try = 0; $try -lt 4 -and -not $ok; $try++) { $r = Xfer (Frame 'W' (0x800 + $p * 256) $d) 1 200; $ok = $r.Length -eq 1 -and $r[0] -eq 0x79 }
    if (-not $ok) { Write-Host "page $p FAILED"; $sp.Close(); exit 1 }
    Write-Host -NoNewline '.'
}
Write-Host ''
Write-Host 'verifying ...'
for ($o = 0; $o -lt $img.Length; $o += 128) {
    $n = [Math]::Min(128, $img.Length - $o)
    $req = [byte[]](0x5A, 0x52, ((0x800 + $o) -band 0xFF), ((0x800 + $o) -shr 8), $n, 0)
    $s = 0; for ($i = 1; $i -lt 6; $i++) { $s = ($s + $req[$i]) -band 0xFF }
    $r = $null
    for ($try = 0; $try -lt 3; $try++) { $r = Xfer ([byte[]]($req + [byte]$s)) ($n + 2) 50; if ($r.Length -eq $n + 2 -and $r[0] -eq 0x79) { break } }
    if ($r.Length -ne $n + 2 -or $r[0] -ne 0x79) { Write-Host "read at $o failed"; $sp.Close(); exit 1 }
    for ($i = 0; $i -lt $n; $i++) { if ($r[1 + $i] -ne $img[$o + $i]) { Write-Host "MISMATCH at 0x$('{0:X}' -f (0x800 + $o + $i))"; $sp.Close(); exit 1 } }
}
Write-Host 'verified. starting the screen firmware ...'
$null = Xfer (Frame 'G' 0) 1 50
Start-Sleep -Milliseconds 1500
$who = Ping
Write-Host "U8 says: $(if ($who) { $who } else { 'nothing' })"
$sp.Close()
if ($who -like 'A*') { Write-Host 'DONE - the screen firmware runs.' } else { Write-Host 'written, but the screen firmware does not answer yet' }
