# Loads ledscan.bin into U8's RAM through the Pico (picorvd_probe.uf2, 'L' command) and then
# shows, once a second, which U8 pin is currently sending the LED signal ('S' command).
# Watch the keyboard: when the LEDs light up, note the pin shown here. Ctrl+C to stop.
param([string]$Port = 'COM5',
      [string]$Bin = 'C:\Users\ASUS-ROG\AppData\Local\Temp\claude\50e52ce2f0736f3b\e69a2f05-5611-485d-b943-6b41cd94db6e\scratchpad\ledscan\ledscan.bin')

function Read-Until($sp, $secs) {
    $buf = ''; $deadline = (Get-Date).AddSeconds($secs)
    while ((Get-Date) -lt $deadline -and $buf -notmatch "END`r`n") { $buf += $sp.ReadExisting(); Start-Sleep -Milliseconds 20 }
    return $buf
}

$img = [IO.File]::ReadAllBytes($Bin)
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 500; $sp.WriteTimeout = 2000
$sp.Open()
try {
    Start-Sleep -Milliseconds 300
    $sp.DiscardInBuffer()
    $hdr = [byte[]](0x4C, ($img.Length -band 0xFF), (($img.Length -shr 8) -band 0xFF))
    $sp.Write($hdr, 0, 3); $sp.Write($img, 0, $img.Length)
    $out = Read-Until $sp 10
    $loaded = ($out -split "`r?`n") | Where-Object { $_ -like 'LOAD*' }
    Write-Host ($loaded -join "`n") -ForegroundColor Cyan
    if ($loaded -notmatch 'verify_bad 0 .*resumed 1') { Write-Host 'Load did not verify - stopping.' -ForegroundColor Red; return }
    Write-Host 'LED scan running. WATCH THE KEYBOARD LEDs. (Ctrl+C to stop)' -ForegroundColor Green
    $last = ''
    while ($true) {
        $sp.DiscardInBuffer(); $sp.Write('S')
        $s = Read-Until $sp 3
        if ($s -match 'STAT h(\d) m0 ([0-9a-f]+) pin ([0-9a-f]+) prof ([0-9a-f]+) beat (\d+) dpc ([0-9a-f]+)') {
            $pin = [Convert]::ToInt32($Matches[3], 16); $prof = [Convert]::ToInt32($Matches[4], 16)
            $name = 'P' + [char](65 + ($pin -shr 8)) + ($pin -band 0xFF)
            $state = if ($Matches[2] -eq 'c0de0001') { 'running' } else { "NOT RUNNING (m0=$($Matches[2]))" }
            $txt = "{0:HH:mm:ss}  pin {1}  timing {2}  round {3}  beat {4}  pc {5}  {6}" -f (Get-Date), $name, ($prof -band 0xFF), ($prof -shr 8), $Matches[5], $Matches[6], $state
            if ($txt -ne $last) { Write-Host $txt -ForegroundColor ($(if ($state -eq 'running') { 'Yellow' } else { 'Red' })) }
            $last = $txt
        } else { Write-Host "no status: $($s.Trim())" -ForegroundColor Red }
        Start-Sleep -Milliseconds 700
    }
} finally { $sp.Close() }
