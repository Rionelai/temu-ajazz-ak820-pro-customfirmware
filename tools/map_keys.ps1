# One-shot key mapping: flash the key finder onto U7 (needle trick), then run the guided mapping.
param([string]$Wchisp = 'wchisp')
& (Join-Path $PSScriptRoot 'flash_u7.ps1') -Hex (Join-Path $PSScriptRoot '..\firmware\out\keyfinder.hex') `
    -Wchisp $Wchisp -Then (Join-Path $PSScriptRoot 'kf_map.ps1')
