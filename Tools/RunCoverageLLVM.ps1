# clang-cl でビルドした計装バイナリから、分岐 (C1) / 条件・MC/DC (C2 以上) まで測る。
#
#   .\Tools\RunCoverageLLVM.ps1                       # build/Coverage を計測
#   .\Tools\RunCoverageLLVM.ps1 -BuildDir build/Cov2
#
# 前提:
#   - Visual Studio インストーラーの «C++ Clang compiler for Windows» が入っていること
#   - coverage preset をビルド済みであること (VS Code タスク "Coverage: Build (clang-cl)")
#     このスクリプトはビルドしない。ビルドはターミナルからではなく VS Code / VS から行う。
#
# WHY OpenCppCoverage と別系統にするか: MSVC には分岐を数える機構が無い。行 (C0) までなら
#     RunCoverage.ps1 の方が速く、開発機に clang を要求しない。«普段は C0、分岐まで見たい日は
#     こちら» という 2 段構えにして、日常のループへ clang-cl ビルドのコストを持ち込まない。

[CmdletBinding()]
param(
    [string]$BuildDir  = 'build/Coverage',
    [string]$OutputDir = 'Artifacts/CoverageLLVM'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

# --- LLVM ツールの解決 ------------------------------------------------------
# WHY PATH より CMakeCache を先に見るか: llvm-profdata / llvm-cov は «計装したときの clang と
#     同じ版» でなければならない。版が違うと «unsupported instrumentation profile format
#     version» で落ちるか、静かに数字がずれる。PATH に別の LLVM が入っている環境で
#     取り違えないよう、まず «実際にビルドに使った clang-cl の隣» を見る。
function Resolve-LlvmTool {
    param([Parameter(Mandatory = $true)][string]$Name)

    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $entry = Select-String -LiteralPath $cache -Pattern '^CMAKE_CXX_COMPILER:[^=]+=(.+)$' | Select-Object -First 1
        if ($entry) {
            $compiler = $entry.Matches[0].Groups[1].Value
            $sibling = Join-Path (Split-Path -Parent $compiler) "$Name.exe"
            if (Test-Path -LiteralPath $sibling) { return $sibling }
        }
    }

    $onPath = Get-Command "$Name.exe" -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    return $null
}

if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    throw @"
カバレッジ用のビルドツリーがありません: $BuildDir
VS Code のタスク "Coverage: Build (clang-cl)" を先に実行してください。
clang-cl が無い場合は Visual Studio インストーラーで «C++ Clang compiler for Windows» を追加します。
"@
}

$llvmProfdata = Resolve-LlvmTool -Name 'llvm-profdata'
$llvmCov      = Resolve-LlvmTool -Name 'llvm-cov'
if (-not $llvmProfdata -or -not $llvmCov) {
    throw 'llvm-profdata / llvm-cov が見つかりません。Visual Studio インストーラーで «C++ Clang compiler for Windows» を追加してください。'
}

# --- 計測対象の収集 ---------------------------------------------------------
$testDir = Join-Path $BuildDir 'Binaries/Debug/Tests'
if (-not (Test-Path -LiteralPath $testDir)) {
    throw "テスト成果物が見つかりません: $testDir (先に coverage preset をビルドしてください)"
}

# WHY 名前で拾うか: スイートを 1 つ足すたびにこのスクリプトへ 1 行足す運用は必ず書き漏れる。
#     Auto スイートの exe 名は FBZZTests<Domain>Auto.exe に固定されており (Docs/conventions/test.md)、
#     OS 状態を触る Manual スイートは ...Manual.exe なのでこのパターンから自然に外れる。
$testExecutables = @(Get-ChildItem -LiteralPath $testDir -Filter 'FBZZTests*Auto.exe' | Sort-Object Name)
if ($testExecutables.Count -eq 0) {
    throw "実行できるテスト exe がありません: $testDir\FBZZTests*Auto.exe"
}

# 計装したコードを «含んでいる» バイナリだけを llvm-cov へ渡す。テスト exe 自身は
# 計装対象外 (CMakeLists.txt の fbzz_instrument_for_coverage を参照) なので渡さない。
$instrumentedNames = @('FBZZMath.dll', 'FBZZPhysics.dll', 'FBZZEngine.dll')
$instrumented = @()
foreach ($name in $instrumentedNames) {
    $path = Join-Path $testDir $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "計装バイナリが見つかりません: $path"
    }
    $instrumented += $path
}

# --- 実行 -------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$rawDir = Join-Path $OutputDir 'raw'
# WHY 毎回消すか: %m はプロファイルの «マージプール» を有効にする。前回の .profraw が
#     残っていると、今回の実行結果へ黙って足し込まれ、消したはずのテストの到達が残り続ける。
if (Test-Path -LiteralPath $rawDir) { Remove-Item -LiteralPath $rawDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $rawDir | Out-Null

# %p = プロセス ID、%m = 計装バイナリごとの識別子。DLL 3 つと exe が同じプロセスに居るため、
# %m が無いと 3 つのモジュールが同じファイルへ書き込もうとして最後の 1 つしか残らない。
$env:LLVM_PROFILE_FILE = Join-Path (Resolve-Path -LiteralPath $rawDir).Path 'fbzz-%p-%m.profraw'

# WHY ctest ではなく exe を直接叩くか: gtest_discover_tests は 1 テスト = 1 プロセスとして
#     CTest へ登録する。ctest 経由だと数千プロセスぶんの .profraw (数 GB) が出る。
#     スイート単位なら 4 プロセスで済み、結果は同じ。
$failed = @()
foreach ($executable in $testExecutables) {
    Write-Host "[Coverage] 実行: $($executable.Name)" -ForegroundColor Cyan
    Push-Location $testDir
    try {
        & $executable.FullName
        if ($LASTEXITCODE -ne 0) { $failed += $executable.Name }
    }
    finally {
        Pop-Location
    }
}

$profdata = Join-Path $OutputDir 'coverage.profdata'
$rawFiles = @(Get-ChildItem -LiteralPath $rawDir -Filter '*.profraw')
if ($rawFiles.Count -eq 0) {
    throw ".profraw が 1 つも出ていません。計装されていないビルドを実行した可能性があります (FBZZ_COVERAGE=ON で configure したか確認)。"
}
& $llvmProfdata merge -sparse -o $profdata @($rawFiles.FullName)
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge が失敗しました (exit $LASTEXITCODE)" }

# --- レポート ---------------------------------------------------------------
# 分母は OpenCppCoverage 側の --sources と同じ 3 つに揃える。ここを揃えないと
# 「C0 は 83%、C1 は 40%」のような比較が «別の母集団同士の比較» になってしまう。
$sourceFilters = @(
    (Join-Path $repoRoot 'Projects\Math')
    (Join-Path $repoRoot 'Projects\Physics')
    (Join-Path $repoRoot 'Projects\Engine\src\Core')
)

# llvm-cov は «先頭 1 つが位置引数、2 つめ以降は -object» という形しか受け付けない。
$objectArguments = @($instrumented[0])
foreach ($binary in ($instrumented | Select-Object -Skip 1)) {
    $objectArguments += @('-object', $binary)
}

$htmlDir = Join-Path $OutputDir 'html'
$lcov    = Join-Path $OutputDir 'lcov.info'
$json    = Join-Path $OutputDir 'coverage.json'
$summary = Join-Path $OutputDir 'summary.md'

& $llvmCov show @objectArguments "-instr-profile=$profdata" `
    -format=html -output-dir=$htmlDir `
    -show-branches=count -show-mcdc -show-line-counts-or-regions `
    -show-instantiation-summary `
    @sourceFilters
if ($LASTEXITCODE -ne 0) { throw "llvm-cov show が失敗しました (exit $LASTEXITCODE)" }

# lcov は Coverage Gutters と ReportGenerator の入力。BRDA レコードに分岐が入る。
& $llvmCov export @objectArguments "-instr-profile=$profdata" -format=lcov @sourceFilters |
    Set-Content -LiteralPath $lcov -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "llvm-cov export (lcov) が失敗しました (exit $LASTEXITCODE)" }

# JSON は要約用。totals に line / branch / region / mcdc がそのまま入っている。
& $llvmCov export @objectArguments "-instr-profile=$profdata" -format=text -summary-only @sourceFilters |
    Set-Content -LiteralPath $json -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw "llvm-cov export (json) が失敗しました (exit $LASTEXITCODE)" }

& (Join-Path $PSScriptRoot 'CoverageReportLLVM.ps1') -JsonPath $json -OutputPath $summary

Write-Host ''
Write-Host "HTML レポート (分岐・MC/DC 付き): $(Join-Path $htmlDir 'index.html')"
Write-Host "lcov (Coverage Gutters 用)      : $lcov"

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Warning "失敗したスイート: $($failed -join ', ') (カバレッジは «失敗も含めた実行» の結果です)"
    exit 1
}
