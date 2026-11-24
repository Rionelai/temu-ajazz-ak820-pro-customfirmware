# Records every line the key-finder firmware types into this window, with a timestamp,
# into keymap_log.txt. Keep this window focused while pressing keys. Ctrl+C to stop.
$log = Join-Path $PSScriptRoot 'keymap_log.txt'
Write-Host "Logging to $log" -ForegroundColor Cyan
while ($true) {
    $line = [Console]::ReadLine()
    if ($null -eq $line) { break }
    $entry = "{0:HH:mm:ss.fff}  {1}" -f (Get-Date), $line
    Add-Content -Path $log -Value $entry
    Write-Host "  saved: $line" -ForegroundColor DarkGray
}
