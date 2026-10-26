# Rebuild the checked-in epilogue (hdr_tone.spv) with an existing DirectX Shader Compiler and the
# SPIRV-Tools next to it (the Vulkan SDK's Bin folder has both); no downloads.
# DXC's library profile declares capabilities and extensions the epilogue does not use (ray
# queries, interlock, ...). They would pass into the shader it is linked into and fail on devices
# without them, so only Shader, Linkage (removed by the linker) and Int16 stay.
param([Parameter(Mandatory)][string]$Dxc, [string]$Tools = (Split-Path -Parent $Dxc))
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $raw = Join-Path ([IO.Path]::GetTempPath()) 'hdr_tone_raw.spv'
    & $Dxc -spirv '-fspv-target-env=vulkan1.3' -Vd -enable-16bit-types -T lib_6_6 -I renodx `
        hdr_tone.hlsl -Fo $raw
    if ($LASTEXITCODE -ne 0) { throw 'HDR epilogue compilation failed' }
    $keep = 'Shader', 'Linkage', 'Int16'
    $text = & (Join-Path $Tools 'spirv-dis.exe') $raw | Where-Object {
        if ($_ -match '^\s*OpCapability (\w+)') { return $keep -contains $Matches[1] }
        return $_ -notmatch '^\s*OpExtension '
    }
    $asm = Join-Path ([IO.Path]::GetTempPath()) 'hdr_tone.spvasm'
    Set-Content -LiteralPath $asm -Value $text -Encoding ascii
    & (Join-Path $Tools 'spirv-as.exe') --target-env vulkan1.3 $asm -o hdr_tone.spv
    if ($LASTEXITCODE -ne 0) { throw 'HDR epilogue assembly failed' }
} finally { Pop-Location }
