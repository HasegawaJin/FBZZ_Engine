# @file    VsEnvironment.ps1
# @brief   Resolve the same x64 Visual Studio compiler host for every build caller.
# @author  Hasegawa Jin
# @date    2026-09-17

# @return The actual supported OS architecture spelling used by Microsoft.Cpp props.
# @note ARM64 keeps the existing x64-host tool policy; availability is checked after vcvars64 resolves the installation.
function Resolve-VisualStudioOperatingSystemArchitecture([string] $architecture) {
    switch ($architecture) {
        'X64' { return 'AMD64' }
        'Arm64' { return 'ARM64' }
        default { throw "Unsupported Visual Studio build operating-system architecture: $architecture" }
    }
}

# @note Changes only this invocation's environment; no user or machine settings are written.
# @note Microsoft.Cpp.ToolsetLocation.props downgrades PreferredToolArchitecture=x64 when both OS architecture environment variables are absent.
function Import-VisualStudioEnvironment {
    $actualArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $env:PROCESSOR_ARCHITECTURE = Resolve-VisualStudioOperatingSystemArchitecture $actualArchitecture
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vsWhere)) {
        throw "vswhere.exe が見つかりません: $vsWhere`nVisual Studio と C++ ワークロードをインストールしてください。"
    }
    # @note Visual Studio discovery and developer-environment setup remain in this single boundary.
    $installPath = & $vsWhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -Last 1
    if ([string]::IsNullOrWhiteSpace($installPath)) {
        throw 'C++ ワークロードを持つ Visual Studio が見つかりません。'
    }
    $vcvars = Join-Path $installPath 'VC/Auxiliary/Build/vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) {
        throw "vcvars64.bat が見つかりません: $vcvars"
    }
    # @note Captured variables belong to the current PowerShell process and its future children only.
    $captured = & cmd.exe /d /c "call `"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvars64.bat の実行に失敗しました: $vcvars"
    }
    foreach ($line in $captured) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            Set-Item -LiteralPath "Env:$name" -Value $value
        }
    }
    $hostCompiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    if (-not (Test-Path -LiteralPath $hostCompiler -PathType Leaf)) {
        throw "Resolved Visual Studio installation has no x64-host x64 compiler: $hostCompiler"
    }
    $env:PreferredToolArchitecture = 'x64'
    Write-Host "[VsEnvironment] os=$actualArchitecture host=x64 target=x64"
}
