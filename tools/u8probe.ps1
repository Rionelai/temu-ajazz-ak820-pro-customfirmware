# Talk to the patched picorvd on the Pi Pico (U8 debug probe, research/u8-probe).
#   .\tools\u8probe.ps1                 -> 'i': U8 chip IDs (read-only)
#   .\tools\u8probe.ps1 -Load file.bin  -> load a program into U8 RAM (0x20000400) and run it
#   .\tools\u8probe.ps1 -Stat           -> read the RAM program's mailbox (0x20000300..30C)
param([string]$Port = '', [string]$Load = '', [switch]$Stat, [int]$Wait = 3)
if (-not $Port) {
    $d = Get-CimInstance Win32_PnPEntity | Where-Object { $_.PNPDeviceID -match 'VID_CAFE&PID_4001' -and $_.Name -match '\((COM\d+)\)' } | Select-Object -First 1
    if (-not $d) { Write-Host 'Pico probe not found (USB VID_CAFE&PID_4001)'; exit 1 }
    $Port = [regex]::Match($d.Name, 'COM\d+').Value
}
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 200
$sp.Open(); Start-Sleep -Milliseconds 200; $sp.DiscardInBuffer()
if ($Load) {
    $b = [System.IO.File]::ReadAllBytes((Resolve-Path $Load))
    if ($b.Length -gt 4096) { Write-Host "too big: $($b.Length) > 4096"; exit 1 }
    $hdr = [byte[]](0x4C, ($b.Length -band 0xFF), ($b.Length -shr 8))
    $sp.Write($hdr, 0, 3)
    for ($i = 0; $i -lt $b.Length; $i += 64) { $sp.Write($b, $i, [Math]::Min(64, $b.Length - $i)) }
} elseif ($Stat) { $sp.Write('S') } else { $sp.Write('i') }
$t = [DateTime]::Now.AddSeconds($Wait); $out = ''
while ([DateTime]::Now -lt $t) {
    try { $out += $sp.ReadExisting() } catch {}
    if ($out -match "END\r?\n") { break }
    Start-Sleep -Milliseconds 50
}
$sp.Close()
$out.TrimEnd()
