# @file    VcBuild.ps1
# @brief   VS Code タスクから CMake を叩く唯一の入口。
# @author  Hasegawa Jin
# @date    2026-08-16
#
# @note tasks.json に VS のパスを書かない。VS の場所を知るのは VsEnvironment.ps1 経由のこの入口だけにする。
# @note .bat にしない。cmd.exe はバイトオフセットで seek するため、マルチバイトのコメントで構文が壊れる。

# @note configure <configurePreset>
# @note build <configurePreset> <buildPreset> [追加の cmake 引数...]
# @note test <configurePreset> [追加の ctest 引数...]
# @note run <configurePreset> <exe 名> [追加の実行時引数...]
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('configure', 'build', 'test', 'run')]
    [string] $Verb,

    [Parameter(Mandatory = $true)]
    [string] $ConfigurePreset,

    # @note build では buildPreset、run では実行する exe 名。configure / test では使わない。
    [Parameter(Mandatory = $false)]
    [string] $BuildPreset,

    # @note cmake / ctest / exe へそのまま渡す (--target 等)。
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $CMakeArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# @note CMake の message() は UTF-8、cl.exe はコンソールのコードページで書く。65001 にすると両方が UTF-8 で揃う。
# @note 子プロセス (cmake / cl / ctest) は chcp を引き継ぐ。OutputEncoding は PowerShell 自身の出力のぶん。
chcp 65001 | Out-Null
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# @name CMakePresets.json との対応表
# @note binaryDir は初回 configure の判定と ctest の実行先に要る。preset を足したらここと AgentBuild.ps1 へも足す。
# @note coverage は Ninja の単一構成で CMAKE_BUILD_TYPE=Debug のため、出力は Binaries/Debug/ になる。
$PresetLayout = @{
    'debug'       = @{ BinaryDir = 'build/Debug';       Configuration = 'Debug' }
    'release'     = @{ BinaryDir = 'build/Release';     Configuration = 'Release' }
    'development' = @{ BinaryDir = 'build/Development'; Configuration = 'Development' }
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

# @note Visual Studio 開発者環境の取り込みは AgentBuild.ps1 と共有する VsEnvironment.ps1 が持つ。
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
        # @note 再構成は ZERO_CHECK が自動で行う。明示の configure が要るのは未 configure のときだけ。
        if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'CMakeCache.txt'))) {
            Write-Host "[VcBuild] 未 configure のため初回 configure を実行します: $ConfigurePreset" -ForegroundColor Cyan
            & cmake --preset $ConfigurePreset
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }

        # @note MSBuild のノード並列は 1。ルート CMakeLists.txt が /MP<n> を渡すので、開けると cl.exe (1 本 1〜2GB) が掛け算で増える。
        # @note coverage (Ninja) は /MP が無く並列をジェネレーターが持つため、物理メモリから決める (clang-cl 1 本 1GB 見積り)。
        if ($PresetKey -eq 'coverage') {
            $memoryMB = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
            $jobs = [Math]::Min([Environment]::ProcessorCount - 2, [int]($memoryMB / 1024))
            $jobs = [Math]::Max(2, [Math]::Min(12, $jobs))
        }
        else {
            $jobs = 1
        }

        & cmake --build --preset $BuildPreset --parallel $jobs @CMakeArguments
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        # @note 明示 target のない通常ビルドだけ、成功後に同じ preset の SDK を直列公開する。
        # @see Docs/design/shared-engine-sdk.md
        $hasExplicitTarget = $false
        foreach ($argument in $CMakeArguments) {
            if ($argument -eq '--') { break }
            if ($argument -eq '--target' -or $argument -eq '-t' -or $argument.StartsWith('--target=')) {
                $hasExplicitTarget = $true
                break
            }
        }
        if ($PresetKey -ne 'coverage' -and -not $hasExplicitTarget) {
            & cmake --build --preset $BuildPreset --parallel $jobs --target FBZZSDK
        }
        exit $LASTEXITCODE
    }

    'test' {
        # @note 手動テスト (Window / Cursor) は CTest 未登録なのでここでは走らない。
        # @note --timeout は再 configure していない古いツリー (TIMEOUT プロパティ無し) でも終わらないテストを止めるため。
        # @note --no-tests=error はフィルタの打ち間違いで 0 件のとき全部成功に見せないため。
        & ctest --test-dir $BinaryDirectory -C $Configuration --output-on-failure `
            --timeout 30 --no-tests=error @CMakeArguments
        exit $LASTEXITCODE
    }

    'run' {
        # @note テスト exe の置き場 (構成ごと) はここだけが知る。tasks.json にパスを書かない。
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
