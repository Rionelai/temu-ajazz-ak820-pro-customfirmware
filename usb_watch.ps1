# Live USB watcher: prints every device that appears or disappears, beeps on new ones,
# and shouts if a WCH bootloader (VID 4348 / 1A86) shows up. Stop with Ctrl+C.
$base = @{}
Get-PnpDevice -PresentOnly | ForEach-Object { $base[$_.InstanceId] = $_.FriendlyName }
Write-Host "Watching USB... plug in the keyboard now. (Ctrl+C to stop)" -ForegroundColor Cyan
while ($true) {
    $now = @{}
    Get-PnpDevice -PresentOnly | ForEach-Object { $now[$_.InstanceId] = $_.FriendlyName }
    foreach ($id in $now.Keys) {
        if (-not $base.ContainsKey($id) -and $id -notlike 'SWD\*' -and $id -notlike 'BTH*') {
            $t = Get-Date -Format HH:mm:ss
            if ($id -match 'VID_4348|VID_1A86') {
                Write-Host "$t  >>> WCH BOOTLOADER FOUND! <<<  $($now[$id])  $id" -ForegroundColor Green
                1..3 | ForEach-Object { [console]::Beep(1500, 300) }
            } else {
                Write-Host "$t  NEW:  $($now[$id])  $id" -ForegroundColor Yellow
                [console]::Beep(900, 200)
            }
        }
    }
    foreach ($id in $base.Keys) {
        if (-not $now.ContainsKey($id) -and $id -notlike 'SWD\*' -and $id -notlike 'BTH*') {
            Write-Host "$(Get-Date -Format HH:mm:ss)  gone: $($base[$id])  $id" -ForegroundColor DarkGray
        }
    }
    $base = $now
    Start-Sleep -Milliseconds 300
}
