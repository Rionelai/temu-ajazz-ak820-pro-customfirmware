# Guided key mapping: tells you which key socket to bridge next, waits for the key-finder
# firmware to report the pin pair over its serial port, and saves keymap.csv (key,pair).
# In this window: S = skip the current key, B = go back one key. Ctrl+C to stop (progress is kept).
$keys = @(
  'Esc','F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12','Delete','KnobPress',
  'Grave','1','2','3','4','5','6','7','8','9','0','Minus','Equal','Backspace','Home',
  'Tab','Q','W','E','R','T','Y','U','I','O','P','LBracket','RBracket','Backslash','PgUp',
  'CapsLock','A','S','D','F','G','H','J','K','L','Semicolon','Quote','Enter','PgDn',
  'LShift','Z','X','C','V','B','N','M','Comma','Period','Slash','RShift','Up',
  'LCtrl','Win','LAlt','Space','RAlt','Fn','RCtrl','Left','Down','Right')
$csv = Join-Path $PSScriptRoot '..\keymap.csv'
$map = [ordered]@{}
if (Test-Path $csv) { Import-Csv $csv | ForEach-Object { $map[$_.key] = $_.pair } }

function Open-Kbd {
    while ($true) {
        $port = Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Ports'" -ErrorAction SilentlyContinue |
            Where-Object { $_.PNPDeviceID -match 'VID_1209&PID_0001' -and $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object { $Matches[1] } | Select-Object -First 1
        if ($port) {
            $sp = New-Object System.IO.Ports.SerialPort $port, 115200
            $sp.DtrEnable = $true; $sp.ReadTimeout = 100; $sp.NewLine = "`n"
            try { $sp.Open(); Write-Host "Keyboard serial port $port open." -ForegroundColor Green; return $sp } catch { }
        }
        Write-Host 'Waiting for the keyboard serial port...' -ForegroundColor DarkGray; Start-Sleep 1
    }
}
function Save { $map.GetEnumerator() | ForEach-Object { [pscustomobject]@{ key = $_.Key; pair = $_.Value } } | Export-Csv $csv -NoTypeInformation }

Write-Host 'Back of the board, USB port at the TOP: the layout is mirrored, so Esc is the TOP-RIGHT socket.' -ForegroundColor Cyan
$sp = Open-Kbd
$i = 0
while ($i -lt $keys.Count -and $map.Contains($keys[$i])) { $i++ }          # resume where we stopped
while ($i -lt $keys.Count) {
    $k = $keys[$i]
    if ($map.Contains($k)) { $i++; continue }                               # already mapped earlier
    Write-Host ("[{0}/{1}]  Bridge the socket of:  {2}" -f ($i + 1), $keys.Count, $k) -ForegroundColor Yellow
    $got = $null
    while (-not $got) {
        if ([Console]::KeyAvailable) {
            $c = [Console]::ReadKey($true).KeyChar
            if ($c -eq 's') { $got = 'skip' } elseif ($c -eq 'b' -and $i -gt 0) { $i--; $map.Remove($keys[$i]); Save; $got = 'back' }
        }
        try { $line = $sp.ReadLine().Trim() } catch [System.TimeoutException] { continue } catch { $sp = Open-Kbd; continue }
        Add-Content (Join-Path $PSScriptRoot '..\keymap_log.txt') ("{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $line)
        if ($line -match '^([ab]\d+-[ab]\d+)$') { $got = $Matches[1] }
        elseif ($line -match '^keyfinder|^idle') { Write-Host "  $line" -ForegroundColor DarkGray }
    }
    if ($got -eq 'back') { continue }
    if ($got -eq 'skip') { Write-Host "  skipped $k" -ForegroundColor DarkGray; $i++; continue }
    $dupe = ($map.GetEnumerator() | Where-Object { $_.Value -eq $got } | Select-Object -First 1)
    if ($dupe) { Write-Host "  $got is already $($dupe.Key)! (B = back if you bridged the wrong one)" -ForegroundColor Red }
    $map[$k] = $got; Save
    Write-Host "  $k = $got" -ForegroundColor Green
    $i++
    Start-Sleep -Milliseconds 300
}
Write-Host "All done - saved to $csv" -ForegroundColor Green
