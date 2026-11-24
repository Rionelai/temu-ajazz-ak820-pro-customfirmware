# Update the keyboard firmware WITHOUT opening the keyboard, through the USB IAP updater
# (firmware/iap) that the first install put at the start of U7's flash.
#   1. Run:  .\tools\update.ps1                (default: firmware\out\keyboard.bin)
#   2. On the keyboard hold Fn+Esc for 3 seconds (or hold Esc while plugging it in).
#   3. The script finds the updater, erases, writes, verifies and starts the new firmware.
# Talks through WCH's CH375DLL64.dll (installed with the CH372DRV driver), WCH IAP protocol:
#   ERASE 0x81 / PROM 0x80 / VERIFY 0x82 / END 0x83 on USB endpoint 2, 2-byte replies, 0 = OK.
param([string]$Bin = (Join-Path $PSScriptRoot '..\firmware\out\keyboard.bin'),
      [uint32]$AppStart = 0x2000)
$ErrorActionPreference = 'Stop'
$Bin = (Resolve-Path $Bin).Path
$img = [IO.File]::ReadAllBytes($Bin)
if ($img.Length -lt 256 -or $img.Length -gt (0x70000 - $AppStart)) { throw "Unexpected firmware size $($img.Length) bytes" }
if ($Bin -match 'full\.bin$') { throw 'full.bin contains the updater itself - use keyboard.bin (or the app .bin) here' }

Add-Type -Namespace WCH -Name Dll -MemberDefinition @'
[DllImport("CH375DLL64.dll")] public static extern System.IntPtr CH375OpenDevice(uint iIndex);
[DllImport("CH375DLL64.dll")] public static extern void CH375CloseDevice(uint iIndex);
[DllImport("CH375DLL64.dll")] public static extern bool CH375SetTimeout(uint iIndex, uint iWriteTimeout, uint iReadTimeout);
[DllImport("CH375DLL64.dll")] public static extern bool CH375GetDeviceDescr(uint iIndex, byte[] oBuffer, ref uint ioLength);
[DllImport("CH375DLL64.dll")] public static extern bool CH375WriteEndP(uint iIndex, uint iPipeNum, byte[] iBuffer, ref uint ioLength);
[DllImport("CH375DLL64.dll")] public static extern bool CH375ReadEndP(uint iIndex, uint iPipeNum, byte[] oBuffer, ref uint ioLength);
'@

function Find-Updater {
    for ($i = 0; $i -lt 16; $i++) {
        $h = [WCH.Dll]::CH375OpenDevice($i)
        if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr](-1)) { continue }
        $d = New-Object byte[] 18; [uint32]$n = 18
        if ([WCH.Dll]::CH375GetDeviceDescr($i, $d, [ref]$n) -and $d[8] -eq 0x48 -and $d[9] -eq 0x43) { return $i }  # VID 4348 = our IAP
        [WCH.Dll]::CH375CloseDevice($i)                                                                        # (ROM bootloader is 1A86)
    }
    return -1
}
function Cmd([uint32]$dev, [byte[]]$pkt, [switch]$NoReply) {
    [uint32]$n = $pkt.Length
    if (-not [WCH.Dll]::CH375WriteEndP($dev, 2, $pkt, [ref]$n) -or $n -ne $pkt.Length) { throw "USB write failed (cmd 0x{0:X2})" -f $pkt[0] }
    if ($NoReply) { return }
    $r = New-Object byte[] 64; [uint32]$m = 64
    if (-not [WCH.Dll]::CH375ReadEndP($dev, 2, $r, [ref]$m) -or $m -lt 1) { throw "no reply (cmd 0x{0:X2})" -f $pkt[0] }
    if ($r[0] -ne 0) { throw ("updater error 0x{0:X2} on cmd 0x{1:X2}" -f $r[0], $pkt[0]) }
}

Write-Host "Firmware: $Bin ($($img.Length) bytes)" -ForegroundColor Cyan
Write-Host 'Now hold Fn+Esc on the keyboard for 3 seconds (or hold Esc while plugging it in)...' -ForegroundColor Yellow
$dev = -1
while ($dev -lt 0) { Start-Sleep -Milliseconds 300; $dev = Find-Updater }
[console]::Beep(1500, 200)
Write-Host "Updater found (device #$dev). Updating - do not unplug!" -ForegroundColor Green
[void][WCH.Dll]::CH375SetTimeout($dev, 5000, 5000)
try {
    $a = [BitConverter]::GetBytes($AppStart)
    Write-Host '  erasing...'
    Cmd $dev ([byte[]](0x81, 0) + $a)
    Write-Host '  writing...'
    for ($o = 0; $o -lt $img.Length; $o += 62) {
        $len = [Math]::Min(62, $img.Length - $o)
        Cmd $dev ([byte[]](0x80, $len) + $img[$o..($o + $len - 1)])
        if ($o % 2480 -eq 0) { Write-Host ("    {0,3}%" -f [int](100 * $o / $img.Length)) }
    }
    Cmd $dev ([byte[]](0x80, 0))                                   # flush the last partial block
    Write-Host '  verifying...'
    for ($o = 0; $o -lt $img.Length; $o += 56) {
        $len = [Math]::Min(56, $img.Length - $o)
        Cmd $dev ([byte[]](0x82, $len) + [BitConverter]::GetBytes([uint32]($AppStart + $o)) + $img[$o..($o + $len - 1)])
    }
    Cmd $dev ([byte[]](0x83, 0)) -NoReply                          # start the new firmware
    Write-Host 'UPDATE OK - the keyboard restarts with the new firmware.' -ForegroundColor Green
} catch {
    Write-Host "UPDATE FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'The updater stays in update mode: just run this script again.' -ForegroundColor Yellow
} finally { [WCH.Dll]::CH375CloseDevice($dev) }
