# Read-only full dump of U8 via picorvd_probe.uf2 ('m' command). Saves the raw text log
# and one .bin per memory range into .\u8_dump\. Never writes to the chip.
param([string]$Port = 'COM5', [string]$OutDir = (Join-Path $PSScriptRoot 'u8_dump'))

New-Item -ItemType Directory -Force $OutDir | Out-Null
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 500; $sp.WriteTimeout = 1500
$sp.Open()
try {
    Start-Sleep -Milliseconds 300
    $sp.DiscardInBuffer()
    $sp.Write('m')
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds(120); $last = Get-Date
    while ((Get-Date) -lt $deadline) {
        $chunk = $sp.ReadExisting()
        if ($chunk) { [void]$sb.Append($chunk); $last = Get-Date }
        if ($sb.ToString().Contains("END`r`n")) { break }
        if (((Get-Date) - $last).TotalSeconds -gt 15) { Write-Warning 'no data for 15 s, stopping'; break }
        Start-Sleep -Milliseconds 30
    }
} finally { $sp.Close() }

$text = $sb.ToString()
Set-Content -Path (Join-Path $OutDir 'dump_log.txt') -Value $text -Encoding ascii
# Rebuild binaries per range; words that failed to read are written as 0xEE bytes.
$name = $null; $bytes = $null; $summary = @()
foreach ($ln in $text -split "`r?`n") {
    if ($ln -match '^RANGE (\S+) ([0-9a-f]{8}) ([0-9a-f]{8})') {
        if ($name) { [IO.File]::WriteAllBytes((Join-Path $OutDir "$name.bin"), $bytes.ToArray()) }
        $name = $Matches[1]; $bytes = New-Object System.Collections.Generic.List[byte]; $ok = 0; $bad = 0
        $summary += $name
    } elseif ($ln -match '^W ([0-9a-f]{8})((?: [0-9a-f]{8})+) E([0-9a-f]{2})') {
        $words = $Matches[2].Trim() -split ' '; $err = [Convert]::ToInt32($Matches[3], 16)
        for ($k = 0; $k -lt $words.Count; $k++) {
            if ($err -band (1 -shl $k)) { 1..4 | ForEach-Object { $bytes.Add(0xEE) } }
            else { [BitConverter]::GetBytes([Convert]::ToUInt32($words[$k], 16)) | ForEach-Object { $bytes.Add($_) } }
        }
    }
}
if ($name) { [IO.File]::WriteAllBytes((Join-Path $OutDir "$name.bin"), $bytes.ToArray()) }
"Saved to $OutDir"
Get-ChildItem $OutDir | Select-Object Name, Length | Format-Table -AutoSize | Out-String
