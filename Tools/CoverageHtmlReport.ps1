# ReportGenerator で HTML レポート・バッジ・lcov を作る。
#
# WHY OpenCppCoverage の HTML があるのに足すか: 素の HTML には «今回の数字» しか無い。
#     ReportGenerator は同じ Cobertura XML から次の 3 つを追加で出す。
#       - 履歴グラフ  -historydir に測定を貯め、コミットを跨いだ推移を折れ線で描く
#       - lcov.info   VS Code の Coverage Gutters がエディター上に色を出すための入力
#       - バッジ      README へ貼れる SVG
#     «今 何 % か» ではなく «前より下がっていないか» を見るための道具。
#
# NOTE Risk Hotspots (循環的複雑度 × 低カバレッジ) は出ない。ReportGenerator が
#      複雑度を読めるのは dotCover / OpenCover 形式のときだけで、OpenCppCoverage の
#      Cobertura は complexity="0" しか書かないため。llvm-cov の lcov も同様。
#      «次にどこを触るか» を出すのは Tools/CoverageReportLLVM.ps1 の
#      «未到達の分岐が多いファイル» の方。
#
# WHY 率の正はこちらではないか: ReportGenerator は同じファイルが複数のパッケージ
#     (DLL / EXE) に現れると 1 つへマージするため、OpenCppCoverage の集計と数値が
#     わずかにずれることがある。CI のしきい値判定が読むのは CoverageReport.ps1 が出す
#     summary.md の方で、こちらは «見るための» レポートに徹する。
#
#   .\Tools\CoverageHtmlReport.ps1 -CoberturaPath Artifacts\Coverage\coverage.xml `
#                                  -OutputDir    Artifacts\Coverage\report
#
# 事前に 1 度だけ:  dotnet tool install -g dotnet-reportgenerator-globaltool

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CoberturaPath,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [string]$HistoryDir,
    [string]$Title = 'FBZZ Engine'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $CoberturaPath)) {
    throw "Cobertura XML が見つかりません: $CoberturaPath"
}

# dotnet global tool は %USERPROFILE%\.dotnet\tools へ入る。インストーラーが書いた PATH は
# 起動済みの VS Code へ伝播しないので、PATH に無くても既定の導入先まで見に行く。
function Find-ReportGenerator {
    $command = Get-Command reportgenerator -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $wellKnown = Join-Path $env:USERPROFILE '.dotnet\tools\reportgenerator.exe'
    if (Test-Path -LiteralPath $wellKnown) { return $wellKnown }

    return $null
}

$reportGenerator = Find-ReportGenerator
if (-not $reportGenerator) {
    # WHY 失敗させないか: これは «見せ方» の追加であって計測ではない。未導入というだけで
    #     カバレッジ計測そのものを失敗扱いにすると、CI と手元の意味がずれる。
    Write-Warning 'reportgenerator が見つかりません。HTML レポート・バッジ・lcov の生成をとばします。'
    Write-Host   '  導入 (1 度だけ):  dotnet tool install -g dotnet-reportgenerator-globaltool'
    return
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# 履歴の 1 点に付ける名前。後からグラフのどの点がどのコミットか読めるようにする。
$tag = (Get-Date -Format 'yyyy-MM-dd_HH-mm-ss')
$shortSha = & git rev-parse --short HEAD 2>$null
if ($LASTEXITCODE -eq 0 -and $shortSha) { $tag = "${tag}_$($shortSha.Trim())" }

$arguments = @(
    "-reports:$CoberturaPath"
    "-targetdir:$OutputDir"
    '-reporttypes:Html;Badges;MarkdownSummaryGithub;lcov'
    "-title:$Title"
    "-tag:$tag"
    '-verbosity:Warning'
)
if ($HistoryDir) {
    New-Item -ItemType Directory -Force -Path $HistoryDir | Out-Null
    $arguments += "-historydir:$HistoryDir"
}

& $reportGenerator @arguments
if ($LASTEXITCODE -ne 0) {
    throw "reportgenerator が失敗しました (exit $LASTEXITCODE)"
}

Write-Host ''
Write-Host "HTML レポート  : $(Join-Path $OutputDir 'index.html')"
Write-Host "バッジ (SVG)   : $(Join-Path $OutputDir 'badge_linecoverage.svg')"
Write-Host "lcov (Gutters) : $(Join-Path $OutputDir 'lcov.info')"
