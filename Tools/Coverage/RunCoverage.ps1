# 手元でカバレッジを取る。CI と同じ計測対象・同じ要約を出す。
#
#   .\Tools\Coverage\RunCoverage.ps1                     # build/Debug を計測
#   .\Tools\Coverage\RunCoverage.ps1 -BuildDir build/ci  # 別のビルドディレクトリ
#
# 事前に OpenCppCoverage が要る:  winget install OpenCppCoverage.OpenCppCoverage
# ビルド自体は Visual Studio / VSCode から済ませておくこと (このスクリプトはビルドしない)。

[CmdletBinding()]
param(
    [string]$BuildDir  = 'build/Debug',
    [string]$Config    = 'Debug',
    [string]$OutputDir = 'Artifacts/Coverage'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

$openCppCoverage = (Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue).Source
if (-not $openCppCoverage) {
    # インストーラーが書いた PATH は起動済みプロセスに伝播しない。VSCode の再起動を
    # 強いないよう、レジストリの PATH と既定の導入先まで見に行く。
    $env:Path = @(
        [Environment]::GetEnvironmentVariable('Path', 'Machine'),
        [Environment]::GetEnvironmentVariable('Path', 'User')
    ) -join ';'
    $openCppCoverage = (Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue).Source
}
if (-not $openCppCoverage) {
    $fallback = Join-Path $env:ProgramFiles 'OpenCppCoverage\OpenCppCoverage.exe'
    if (Test-Path $fallback) { $openCppCoverage = $fallback }
}
if (-not $openCppCoverage) {
    throw 'OpenCppCoverage.exe が見つかりません。winget install OpenCppCoverage.OpenCppCoverage で入れること。'
}
if (-not (Test-Path $BuildDir)) {
    throw "ビルドディレクトリが見つかりません: $BuildDir (先に VS / VSCode でビルドしてください)"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$cobertura = Join-Path $OutputDir 'coverage.xml'
$html      = Join-Path $OutputDir 'html'
$summary   = Join-Path $OutputDir 'summary.md'
$reportDir = Join-Path $OutputDir 'report'
# 履歴は測定ごとに 1 ファイル増えていく。消すと推移グラフの過去分が丸ごと消える。
$historyDir = Join-Path $OutputDir 'history'

# --cover_children: ctest が起動する各テスト exe まで追う。
#
# 計測対象は «テストで守ると決めた領域» だけに絞る。Renderer や ImGui のパネルまで
# 混ぜると、そもそもテストしない領域が分母に入って数値が意味を失う。
#
# Editor はモジュール単位で入れる。Util / Import / Ai / GraphEditor / Tools は
# 判定・変換・直列化といった «目視では追えない» ロジックで、テストで守る対象。
# src\Panels は ImGui の描画そのものなので入れない (自動テストの対象外)。
& $openCppCoverage `
    --cover_children `
    --sources "Projects\Math" `
    --sources "Projects\Physics" `
    --sources "Projects\Engine\src\Core" `
    --sources "Projects\Engine\src\Asset" `
    --sources "Projects\Editor\src\Util" `
    --sources "Projects\Editor\src\Import" `
    --sources "Projects\Editor\src\Ai" `
    --sources "Projects\Editor\src\GraphEditor" `
    --sources "Projects\Editor\src\Tools" `
    --excluded_sources "Projects\Tests" `
    --excluded_sources "ThirdParty" `
    --export_type "cobertura:$cobertura" `
    --export_type "html:$html" `
    -- ctest --test-dir $BuildDir -C $Config --output-on-failure

# OpenCppCoverage の出す «ドライブ文字 + 開発機の絶対パス» を、リポジトリ相対へ直す。
# これを挟まないと ReportGenerator も Coverage Gutters もソースを開けない。
& (Join-Path $PSScriptRoot 'NormalizeCoverageXml.ps1') -CoberturaPath $cobertura

& (Join-Path $PSScriptRoot 'CoverageReport.ps1') -CoberturaPath $cobertura -OutputPath $summary

& (Join-Path $PSScriptRoot 'CoverageHtmlReport.ps1') `
    -CoberturaPath $cobertura -OutputDir $reportDir -HistoryDir $historyDir

Write-Host ''
Write-Host "OpenCppCoverage の HTML: $(Join-Path $html 'index.html')"
