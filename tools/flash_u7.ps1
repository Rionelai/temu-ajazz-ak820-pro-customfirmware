# Flash a firmware onto U7 (WCH CH584) through its ROM bootloader using wchisp.
#  1. Run this script.  2. Hold a needle (wired to GND) on U7 pin 26 (PB22) and plug in USB.
#  3. When it beeps, lift the needle. It erases, writes, verifies and resets the chip.
# The ROM bootloader cannot be overwritten, so a bad image is always recoverable this way.
# Optional -Then runs another script afterwards (e.g. .\tools\kf_map.ps1 for the key finder).
param(
    # full.bin = USB updater (IAP) + keyboard firmware: the one-time install through the ROM
    # bootloader. After that, update with tools\update.ps1 (no opening the keyboard).
    [string]$Hex = (Join-Path $PSScriptRoot '..\firmware\out\full.bin'),
    [string]$Wchisp = 'wchisp',      # path to wchisp.exe (nightly build, with CH375DLL64.dll next to it)
    [string]$Then = ''
)
$Hex = (Resolve-Path $Hex).Path
$local = Join-Path $PSScriptRoot '..\.toolchain\wchisp\wchisp.exe'      # repo-local copy (gitignored)
if ($Wchisp -eq 'wchisp' -and (Test-Path $local)) { $Wchisp = (Resolve-Path $local).Path }
Write-Host "Firmware: $Hex" -ForegroundColor Cyan
Write-Host 'Waiting for the U7 bootloader (USB 1A86:55E0)... needle on pin 26, then plug in.' -ForegroundColor Cyan
while ($true) {
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_(1A86|4348)&PID_55E0' }
    if ($dev) {
        [console]::Beep(1500, 300)
        Write-Host "$(Get-Date -Format HH:mm:ss)  Bootloader found - flashing, do not unplug!" -ForegroundColor Yellow
        $out = & $Wchisp flash $Hex 2>&1 | Out-String
        Write-Host $out
        if ($LASTEXITCODE -eq 0 -and $out -notmatch '(?i)\berror\b|failed') { Write-Host 'FLASHED OK.' -ForegroundColor Green; break }
        Write-Host 'Flash did not report success - retrying while the bootloader is present...' -ForegroundColor Red
        Start-Sleep -Milliseconds 800
    } else { Start-Sleep -Milliseconds 250 }
}
if ($Then) { & $Then }
