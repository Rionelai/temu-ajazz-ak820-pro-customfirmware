# Load a RAM program into U8 via the Pico ('L' command) and print the probe's LOADED line.
param([Parameter(Mandatory)][string]$Bin, [string]$Port = 'COM5')
$img = [IO.File]::ReadAllBytes($Bin)
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 500; $sp.WriteTimeout = 2000
$sp.Open()
try {
    Start-Sleep -Milliseconds 300
    $sp.DiscardInBuffer()
    $sp.Write([byte[]](0x4C, ($img.Length -band 0xFF), (($img.Length -shr 8) -band 0xFF)), 0, 3)
    $sp.Write($img, 0, $img.Length)
    $buf = ''; $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and $buf -notmatch "END`r`n") { $buf += $sp.ReadExisting(); Start-Sleep -Milliseconds 20 }
    ($buf -split "`r?`n") | Where-Object { $_ -like 'LOAD*' }
} finally { $sp.Close() }
