# Read-only probe of the keyboard's U8 chip via the patched picorvd firmware (picorvd_probe.uf2).
# Sends one byte; the Pico reads the SWIO line level and a few debug registers and prints them.
# -Cmd p : registers only (no halt).  -Cmd i : halt, read IDs/flash via picorvd's method.
# -Cmd d : halt, try 4 memory-access methods with error codes, then resume.  -Cmd r : resume only.
# -Cmd c : halt, read CPU identity CSRs + ID/config/GPIO/RCC words, then resume.
param([string]$Port = 'COM5', [ValidateSet('p','i','d','r','c')][string]$Cmd = 'p')

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true; $sp.ReadTimeout = 500; $sp.WriteTimeout = 1500
$sp.Open()
try {
    Start-Sleep -Milliseconds 300
    $sp.DiscardInBuffer()
    $sp.Write($Cmd)
    $out = ''; $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and $out -notmatch 'END') {
        $out += $sp.ReadExisting(); Start-Sleep -Milliseconds 50
    }
    if ($out) { $out.Trim() } else { 'NO OUTPUT from Pico' }
} finally { $sp.Close() }
