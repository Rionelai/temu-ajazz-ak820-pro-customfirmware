# Collect every key's pin pair in ANY order: bridge each hot-swap socket once, the script keeps
# the unique pairs in pairs.csv and shows a live count. Key names are worked out afterwards from
# the matrix (row pin + column pin). Ignores the knob pins (b0/b1) and bursts of fake pairs.
param([int]$Expected = 82)
$out = Join-Path $PSScriptRoot '..\pairs.csv'
$pairs = [ordered]@{}
if (Test-Path $out) { Import-Csv $out | ForEach-Object { $pairs[$_.pair] = $_.first_seen } }

function Open-Kbd {
    while ($true) {
        $port = Get-CimInstance Win32_PnPEntity -Filter "PNPClass='Ports'" -ErrorAction SilentlyContinue |
            Where-Object { $_.PNPDeviceID -match 'VID_1209&PID_0001' -and $_.Name -match '\((COM\d+)\)' } |
            ForEach-Object { $Matches[1] } | Select-Object -First 1
        if ($port) {
            $sp = New-Object System.IO.Ports.SerialPort $port, 115200
            $sp.DtrEnable = $true; $sp.ReadTimeout = 100; $sp.NewLine = "`n"
            try { $sp.Open(); Write-Host "Keyboard on $port. Bridge every socket once, any order." -ForegroundColor Green; return $sp } catch { }
        }
        Write-Host 'Waiting for the keyboard (key finder firmware)...' -ForegroundColor DarkGray; Start-Sleep 1
    }
}
function Save { $pairs.GetEnumerator() | ForEach-Object { [pscustomobject]@{ pair = $_.Key; first_seen = $_.Value } } | Export-Csv $out -NoTypeInformation }

$sp = Open-Kbd
# A real bridge is ONE pair followed by >= 0.5 s of quiet. Several pairs closer than 0.5 s are a
# burst (noise or touching two sockets): the whole group is dropped.
$group = New-Object System.Collections.Generic.List[string]
$last = [datetime]::MinValue
while ($true) {
    try { $line = $sp.ReadLine().Trim() } catch [System.TimeoutException] { $line = $null } catch { $sp = Open-Kbd; continue }
    $now = Get-Date
    if ($group.Count -gt 0 -and ($now - $last).TotalMilliseconds -gt 500) {
        if ($group.Count -eq 1) {
            $p = $group[0]
            if (-not $pairs.Contains($p)) {
                $pairs[$p] = $now.ToString('HH:mm:ss'); Save
                Write-Host ("  NEW   {0,-9}  ({1}/{2} found)" -f $p, $pairs.Count, $Expected) -ForegroundColor Green
                [console]::Beep(1200, 80)
            } else { Write-Host ("  again {0}  (already have it)" -f $p) -ForegroundColor DarkGray }
        } else { Write-Host ("  ignored burst of {0} pairs - bridge that socket again" -f $group.Count) -ForegroundColor DarkYellow }
        $group.Clear()
    }
    if (-not $line -or $line -notmatch '^([ab]\d+)-([ab]\d+)$') { continue }
    if ($Matches[1] -in 'b0','b1' -or $Matches[2] -in 'b0','b1') { continue }      # knob pins
    $group.Add($line); $last = $now
}
