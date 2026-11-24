# Waits for U7's WCH ROM bootloader, then flashes a firmware with wchisp (erase + write +
# verify + reset). The ROM bootloader cannot be overwritten, so a bad image is always
# recoverable with the pin-26 needle trick. Afterwards it starts the key logger.
param([string]$Hex = 'C:\Users\ASUS-ROG\AppData\Local\Temp\claude\50e52ce2f0736f3b\e69a2f05-5611-485d-b943-6b41cd94db6e\scratchpad\fw\keyfinder\keyfinder.hex')
$wchisp = 'C:\Users\ASUS-ROG\AppData\Local\Temp\claude\50e52ce2f0736f3b\e69a2f05-5611-485d-b943-6b41cd94db6e\scratchpad\wchisp\n\wchisp-win-x64\wchisp.exe'
$log = Join-Path $PSScriptRoot 'flash_u7.log'
Write-Host "Firmware: $Hex" -ForegroundColor Cyan
Write-Host 'Waiting for U7 bootloader... (needle on pin 26, then plug in the keyboard)' -ForegroundColor Cyan
while ($true) {
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_(1A86|4348)&PID_55E0' }
    if ($dev) {
        [console]::Beep(1500, 300)
        Write-Host "$(Get-Date -Format HH:mm:ss)  Bootloader found - flashing, DON'T unplug!" -ForegroundColor Yellow
        $out = & $wchisp flash $Hex 2>&1 | Out-String
        $out | Tee-Object -FilePath $log -Append | Write-Host
        if ($LASTEXITCODE -eq 0 -and $out -notmatch '(?i)\berror\b|failed') { Write-Host 'FLASHED OK.' -ForegroundColor Green; break }
        Write-Host 'Flash did not report success - retrying while the bootloader is present...' -ForegroundColor Red
        Start-Sleep -Milliseconds 800
    } else { Start-Sleep -Milliseconds 250 }
}
Write-Host ''
Write-Host 'Starting the guided key mapping.' -ForegroundColor Green
& (Join-Path $PSScriptRoot 'kf_map.ps1')
