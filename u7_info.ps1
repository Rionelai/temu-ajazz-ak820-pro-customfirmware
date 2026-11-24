# Waits for U7's WCH ROM bootloader (USB 1A86:55E0 / 4348:55E0) and immediately runs the
# read-only `wchisp info` on it, saving the output to u7_info.txt. Never writes to the chip.
$wchisp = 'C:\Users\ASUS-ROG\AppData\Local\Temp\claude\50e52ce2f0736f3b\e69a2f05-5611-485d-b943-6b41cd94db6e\scratchpad\wchisp\n\wchisp-win-x64\wchisp.exe'   # nightly: works with the official WCH (CH375) driver
$log = Join-Path $PSScriptRoot 'u7_info.txt'
Write-Host 'Waiting for U7 bootloader... (needle on pin 26, then plug in the keyboard). Ctrl+C to stop.' -ForegroundColor Cyan
while ($true) {
    $dev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_(1A86|4348)&PID_55E0' }
    if ($dev) {
        Write-Host "$(Get-Date -Format HH:mm:ss)  Bootloader present: $($dev.InstanceId)  status=$($dev.Status)" -ForegroundColor Green
        [console]::Beep(1500, 300)
        $out = & $wchisp info 2>&1 | Out-String
        $out | Tee-Object -FilePath $log -Append | Write-Host
        if ($out -match 'Chip:|Chip ') { Write-Host 'Chip info read - saved to u7_info.txt' -ForegroundColor Green; break }
        Write-Host 'wchisp could not talk to it yet (driver?). Retrying while it is present...' -ForegroundColor Yellow
        Start-Sleep -Milliseconds 800
    } else { Start-Sleep -Milliseconds 250 }
}
