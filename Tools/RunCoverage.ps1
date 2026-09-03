# 手元でカバレッジを取る。CI と同じ計測対象・同じ要約を出す。
#
#   .\Tools\RunCoverage.ps1                     # build/Debug を計測
#   .\Tools\RunCoverage.ps1 -BuildDir build/ci  # 別のビルドディレクトリ
#
# 事前に OpenCppCoverage が要る:  choco install opencppcoverage
# ビルド自体は Visual Studio / VSCode から済ませておくこと (このスクリプトはビルドしない)。

[CmdletBinding()]
param(
    [string]$BuildDir  = 'build/Debug',
    [string]$Config    = 'Debug',
    [string]$OutputDir = 'Artifacts/Coverage'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if (-not (Get-Command OpenCppCoverage.exe -ErrorAction SilentlyContinue)) {
    throw 'OpenCppCoverage.exe が PATH にありません。choco install opencppcoverage で入れてください。'
}
if (-not (Test-Path $BuildDir)) {
    throw "ビルドディレクトリが見つかりません: $BuildDir (先に VS / VSCode でビルドしてください)"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$cobertura = Join-Path $OutputDir 'coverage.xml'
$html      = Join-Path $OutputDir 'html'
$summary   = Join-Path $OutputDir 'summary.md'

# --cover_children: ctest が起動する各テスト exe まで追う。
# 計測対象は自作の 3 モジュールだけ。Renderer / Editor を混ぜると、テストしないと
# 決めた領域が分母に入って数値が意味を失う。
OpenCppCoverage.exe `
    --cover_children `
    --sources "Projects\Math" `
    --sources "Projects\Physics" `
    --sources "Projects\Engine\src\Core" `
    --excluded_sources "Projects\Tests" `
    --excluded_sources "ThirdParty" `
    --export_type "cobertura:$cobertura" `
    --export_type "html:$html" `
    -- ctest --test-dir $BuildDir -C $Config --output-on-failure

& (Join-Path $PSScriptRoot 'CoverageReport.ps1') -CoberturaPath $cobertura -OutputPath $summary

Write-Host ''
Write-Host "HTML レポート: $(Join-Path $html 'index.html')"
