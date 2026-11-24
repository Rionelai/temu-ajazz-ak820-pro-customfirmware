# ONE-TIME U8 install through the Pico probe (research/u8-probe): writes a .bin into U8's flash
# with the RAM flash writer (flashwr.c). Normally only for firmware\out\u8boot.bin at offset 0;
# after that, U8 is updated over the keyboard's USB (no Pico).
#   .\tools\u8flash.ps1 -Bin .\firmware\out\u8boot.bin [-Offset 0] [-Writer <flashwr.bin>]
param([Parameter(Mandatory)] [string]$Bin, [int]$Offset = 0, [string]$Writer = "$env:TEMP\u8rd\flashwr.bin")
$w = [System.IO.File]::ReadAllBytes((Resolve-Path $Writer))
$p = [System.IO.File]::ReadAllBytes((Resolve-Path $Bin))
if ($w.Length -gt 0x400) { throw "writer too big ($($w.Length) > 1024)" }
$plen = ($p.Length + 3) -band -4
if (0x410 + $plen -gt 4096) { throw "payload too big for one load ($($p.Length) bytes, max 3056)" }
$img = New-Object byte[] (0x410 + $plen)
[Array]::Copy($w, $img, $w.Length)
for ($i = 0; $i -lt $plen; $i++) { $img[0x410 + $i] = if ($i -lt $p.Length) { $p[$i] } else { 0xFF } }
[uint64]$acc = 0
for ($i = 0; $i -lt $plen; $i += 4) { $acc += [BitConverter]::ToUInt32($img, 0x410 + $i) }
$sum = [uint32]($acc % 4294967296)   # (0xFFFFFFFF is -1 in PowerShell 5, so no -band)
$hdr = [BitConverter]::GetBytes([uint32]0x57463855) + [BitConverter]::GetBytes([uint32]$Offset) +
       [BitConverter]::GetBytes([uint32]$p.Length) + [BitConverter]::GetBytes($sum)
[Array]::Copy([byte[]]$hdr, 0, $img, 0x400, 16)
$tmp = Join-Path $env:TEMP 'u8flash_img.bin'
[System.IO.File]::WriteAllBytes($tmp, $img)
Write-Host "writing $($p.Length) bytes to U8 flash offset 0x$('{0:X}' -f $Offset) ..."
& "$PSScriptRoot\u8probe.ps1" -Load $tmp -Wait 3 | Select-String LOADED
Start-Sleep 2
$s = (& "$PSScriptRoot\u8probe.ps1" -Stat -Wait 2) | Select-String STAT
$s
if ("$s" -match 'm0 c0de00d9') { Write-Host 'OK: written and verified' } else { Write-Host 'FAILED (m0 c0deeeee = error; pin = step|page<<8, beat = bad words)'; exit 1 }
