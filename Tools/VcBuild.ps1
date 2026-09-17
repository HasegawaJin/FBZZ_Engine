# FBZZ Engine
# VcBuild.ps1 | Tools
# VS Code タスクから CMake を叩くための唯一の入口。
#
# WHY (絶対パスを排除): 以前は vcvars64.bat の絶対パスを tasks.json へ 20 箇所
#     ハードコードしていた。Visual Studio のエディション (Community/Professional) や
#     世代が変わるだけで全ビルドタスクが同時に壊れ、20 箇所を手で直す必要があった。
#     VS の場所を知っているのはこのファイル 1 つだけ、という状態にする。
#
# WHY (.bat ではなく .ps1): cmd.exe はバッチファイルを「バイトオフセット」で読み進め、
#     goto のたびにファイルを seek し直す。日本語コメントのようなマルチバイト文字が
#     あると seek 位置が文字境界からずれてパーサーが壊れ、コメントのはずの断片が
#     コマンドとして実行される。PowerShell は文字単位で解釈するためこの問題が無い。
#
# 使い方:
#   VcBuild.ps1 configure <configurePreset>
#   VcBuild.ps1 build     <configurePreset> <buildPreset> [追加の cmake 引数...]
#   VcBuild.ps1 test      <configurePreset> [追加の ctest 引数...]
#   VcBuild.ps1 run       <configurePreset> <exe 名>       [追加の実行時引数...]

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('configure', 'build', 'test', 'run')]
    [string] $Verb,

    [Parameter(Mandatory = $true)]
    [string] $ConfigurePreset,

    # build では buildPreset、run では実行する exe 名。configure / test では使わない。
    [Parameter(Mandatory = $false)]
    [string] $BuildPreset,

    # --target 等、cmake / ctest / exe へそのまま渡す追加引数。
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CMakeArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- コンソールを UTF-8 に揃える ---------------------------------------------
# WHY: CMake は message() を UTF-8 で書くが、日本語 Windows のコンソールは CP932 なので
#      VS Code のタスク端末では "-- FBZZ: 荳ｦ蛻・.." と化ける。cl.exe はコンソールの
#      コードページに従って出力する (chcp 932 なら CP932、65001 なら UTF-8 で書くことを
#      実測で確認) ので、ここで 65001 にすれば CMake と MSVC の両方が UTF-8 で揃い、
#      端末側は一様に読める。子プロセス (cmake / cl / ctest) はこの設定を引き継ぐ。
#      [Console]::OutputEncoding は PowerShell 自身の Write-Host のぶん。
chcp 65001 | Out-Null
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# --- CMakePresets.json との対応表 ------------------------------------------
# WHY: 初回の自動 configure 判定 (CMakeCache.txt の有無) と ctest の実行先を知るために
#      binaryDir が要る。preset を追加したらここへも 1 行足すこと。
#      ずれると初回ビルドが「未 configure なのに configure されない」状態になる。
#
#      coverage は Ninja の単一構成ビルド。CMAKE_BUILD_TYPE=Debug なので、CMake が
#      参照する出力先プロパティは他と同じ *_DEBUG 系になり、Binaries/Debug/ へ出る。
$PresetLayout = @{
    'debug'       = @{ BinaryDir = 'build/Debug';       Configuration = 'Debug' }
    'release'     = @{ BinaryDir = 'build/Release';     Configuration = 'Release' }
    'development' = @{ BinaryDir = 'build/Development'; Configuration = 'Development' }
    'sdk'         = @{ BinaryDir = 'build/SDK';         Configuration = 'Development' }
    'coverage'    = @{ BinaryDir = 'build/Coverage';    Configuration = 'Debug' }
}

$PresetKey = $ConfigurePreset.ToLowerInvariant()
if (-not $PresetLayout.ContainsKey($PresetKey)) {
    Write-Host "[VcBuild] 未知の configure preset: $ConfigurePreset" -ForegroundColor Red
    Write-Host "[VcBuild] 既知の preset: $($PresetLayout.Keys -join ', ')"
    Write-Host "[VcBuild] CMakePresets.json へ追加した preset は Tools/VcBuild.ps1 の対応表にも登録してください。"
    exit 1
}
$BinaryDirectory = Join-Path $RepositoryRoot $PresetLayout[$PresetKey].BinaryDir
$Configuration = $PresetLayout[$PresetKey].Configuration

if ($Verb -eq 'build' -and [string]::IsNullOrWhiteSpace($BuildPreset)) {
    Write-Host "[VcBuild] build には buildPreset が必要です。" -ForegroundColor Red
    exit 1
}

# --- Visual Studio 開発者環境の取り込み ------------------------------------
# 手順の本体は VsEnvironment.ps1 (AgentBuild.ps1 と共有する)。
. (Join-Path $PSScriptRoot 'VsEnvironment.ps1')

try {
    Import-VisualStudioEnvironment
}
catch {
    Write-Host "[VcBuild] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Set-Location -LiteralPath $RepositoryRoot

switch ($Verb) {
    'configure' {
        & cmake --preset $ConfigurePreset
        exit $LASTEXITCODE
    }

    'build' {
        # WHY: 以前は全ビルドタスクが Configure タスクへ dependsOn しており、毎回 CMake の
        #      再構成コストを払っていた。Visual Studio ジェネレーターは ZERO_CHECK が
        #      CMakeLists.txt の変更を検知して自動再構成するので、明示的な configure が
        #      要るのは「まだ一度も configure していない時」だけ。それをここで判定する。
        if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'CMakeCache.txt'))) {
            Write-Host "[VcBuild] 未 configure のため初回 configure を実行します: $ConfigurePreset" -ForegroundColor Cyan
            & cmake --preset $ConfigurePreset
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }

        # WHY: --parallel 1 で MSBuild のノード並列を 1 に抑える。ルート CMakeLists.txt が
        #      各プロジェクトへ /MP<n> を渡しているため、ノード並列まで開けると
        #      「プロジェクト数 × /MP」で cl.exe が掛け算に増える。Inspector 等の
        #      /bigobj が要る重い翻訳単位は cl.exe 1 つで 1〜2GB 使うため、
        #      物理メモリを使い切ってマシン全体がスワップに巻き込まれる。
        #
        #      coverage preset だけは Ninja。/MP を渡していないので «並列はジェネレーターが
        #      全部持つ» 側になり、ここで 1 を渡すとビルド全体が本当に直列になる。
        #      物理メモリから同時実行数を決め直す (clang-cl 1 プロセス = 1GB 見積り)。
        if ($PresetKey -eq 'coverage') {
            $memoryMB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
            $jobs = [Math]::Min([Environment]::ProcessorCount - 2, [int]($memoryMB / 1024))
            $jobs = [Math]::Max(2, [Math]::Min(12, $jobs))
        }
        else {
            $jobs = 1
        }

        & cmake --build --preset $BuildPreset --parallel $jobs @CMakeArguments
        exit $LASTEXITCODE
    }

    'test' {
        # gtest_discover_tests() が CTest へ個別テストを登録済みなので、exe を直接叩かず
        # ctest を通す。失敗したテストの出力だけがそのままターミナルへ出る。
        # ManualTest は Window/Cursor の OS 状態を触るため CTest 未登録で、ここでは走らない。
        # 追加引数はそのまま ctest へ渡す (-R で名前を絞る等)。
        #
        # --timeout: 1 件でも «終わらないテスト» があると、そこから先が丸ごと実行されない。
        #   CMake 側でも TIMEOUT プロパティを付けているが、古いビルドツリー
        #   (再 configure していない) には載っていないため、ここでも上限を渡す。
        # --no-tests=error: フィルタの打ち間違いで 0 件になったとき «全部成功» に見せない。
        & ctest --test-dir $BinaryDirectory -C $Configuration --output-on-failure `
            --timeout 30 --no-tests=error @CMakeArguments
        exit $LASTEXITCODE
    }

    'run' {
        # WHY ここで解決するか: テスト成果物の置き場は構成ごとに分かれる。
        #     tasks.json 側に書くと preset を足すたびに全タスクへ同じパスが増える。
        #     「どこに出るか」を知っているのはこのファイルだけ、という状態を保つ。
        if ([string]::IsNullOrWhiteSpace($BuildPreset)) {
            Write-Host "[VcBuild] run には exe 名が必要です。" -ForegroundColor Red
            exit 1
        }

        $executable = Join-Path $BinaryDirectory "Binaries/$Configuration/Tests/$BuildPreset.exe"
        if (-not (Test-Path -LiteralPath $executable)) {
            Write-Host "[VcBuild] 実行ファイルが見つかりません: $executable" -ForegroundColor Red
            Write-Host "[VcBuild] 先に対応するビルドタスクを実行してください。"
            exit 1
        }

        & $executable @CMakeArguments
        exit $LASTEXITCODE
    }
}
