# FBZZ Engine
# VsEnvironment.ps1 | Tools
# Visual Studio 開発者環境 (cl / link / msbuild の PATH) を現在の PowerShell セッションへ取り込む。
#
# WHY 分けたか: VS の場所を知るのは 1 か所だけ、という VcBuild.ps1 の方針を保ったまま、
#     AI 用の入口 (AgentBuild.ps1) からも同じ手順で環境を作るため。dot-source して使う。

function Import-VisualStudioEnvironment {
    # vswhere.exe は VS インストーラーが必ずこの固定パスへ置く、唯一安定した入口。
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vsWhere)) {
        throw "vswhere.exe が見つかりません: $vsWhere`nVisual Studio と C++ ワークロードをインストールしてください。"
    }

    # C++ x64 ツールチェーンを実際に持つインストールだけを候補にし、最新版を選ぶ。
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

    # WHY: vcvars64.bat はバッチでしか環境を作れないため、cmd 側で実行して
    #      その結果の環境変数一式を読み戻し、この PowerShell セッションへ反映する。
    #      こうしないと cl.exe / link.exe が PATH に載らない。
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
}
