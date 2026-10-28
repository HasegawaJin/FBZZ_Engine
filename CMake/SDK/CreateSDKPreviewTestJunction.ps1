# @file    CreateSDKPreviewTestJunction.ps1
# @brief   Create a Windows junction fixture for SDK preview containment tests.
# @author  Hasegawa Jin
# @date    2026-10-05
param(
    [Parameter(Mandatory = $true)][string]$LinkPath,
    [Parameter(Mandatory = $true)][string]$TargetPath
)

$ErrorActionPreference = 'Stop'
# @note Directory junctions exercise Windows reparse-point containment without requiring symlink privileges.
New-Item -ItemType Junction -Path $LinkPath -Target $TargetPath | Out-Null
