# Reads the key-finder firmware's USB serial port (VID 1209 / PID 0001) and logs every line to
# keymap_log.txt. No window focus needed: the keyboard no longer types. Ctrl+C to stop.
$log = Join-Path $PSScriptRoot '..\keymap_log.txt'
Add-Content -Path $log -Value ("---- serial session {0:yyyy-MM-dd HH:mm:ss} ----" -f (Get-Date))
Write-Host "Logging to $log" -ForegroundColor Cyan
while ($true) {
    $port = Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Ports'" -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match 'VID_1209&PID_0001' -and $_.Name -match '\((COM\d+)\)' } |
        ForEach-Object { $Matches[1] } | Select-Object -First 1
    if (-not $port) { Write-Host 'Waiting for the keyboard serial port...' -ForegroundColor DarkGray; Start-Sleep 1; continue }
    Write-Host "Keyboard serial port: $port - press keys / turn the knob now." -ForegroundColor Green
    $sp = New-Object System.IO.Ports.SerialPort $port, 115200
    $sp.DtrEnable = $true; $sp.ReadTimeout = 1000; $sp.NewLine = "`n"
    try {
        $sp.Open()
        while ($true) {
            try { $line = $sp.ReadLine().TrimEnd("`r") } catch [System.TimeoutException] { continue }
            $entry = "{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $line
            Add-Content -Path $log -Value $entry
            Write-Host $entry
        }
    } catch { Write-Host "Port closed ($($_.Exception.Message)) - reconnecting..." -ForegroundColor Yellow }
    finally { if ($sp.IsOpen) { $sp.Close() } }
    Start-Sleep 1
}
