# Cobertura XML から Markdown のカバレッジ要約を作る。
#
# CI と手元で同じ出力にするためスクリプトへ切り出している。
# サードパーティ Action に依存しないのは、ポートフォリオ公開リポジトリで
# 「何が動いているか」を読んで確かめられる状態を保つため。

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CoberturaPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $CoberturaPath)) {
    throw "Cobertura XML が見つかりません: $CoberturaPath"
}

[xml]$report = Get-Content -Path $CoberturaPath -Raw

$linesValid   = [int]$report.coverage.'lines-valid'
$linesCovered = [int]$report.coverage.'lines-covered'
$linePercent  = if ($linesValid -gt 0) { [math]::Round(100.0 * $linesCovered / $linesValid, 2) } else { 0.0 }

# 90% 以上は緑、75% 以上は黄、それ未満は赤。README に貼る数値の目安でもある。
$badge = if ($linePercent -ge 90) { '🟢' } elseif ($linePercent -ge 75) { '🟡' } else { '🔴' }

$builder = [System.Text.StringBuilder]::new()
[void]$builder.AppendLine("## $badge 行カバレッジ $linePercent%")
[void]$builder.AppendLine()
[void]$builder.AppendLine("計測 $linesCovered / $linesValid 行")
[void]$builder.AppendLine()
[void]$builder.AppendLine('| モジュール | 行カバレッジ | 到達 / 全体 |')
[void]$builder.AppendLine('|---|---:|---:|')

foreach ($package in $report.coverage.packages.package) {
    $packageValid   = 0
    $packageCovered = 0

    foreach ($class in $package.classes.class) {
        foreach ($line in $class.lines.line) {
            $packageValid++
            if ([int]$line.hits -gt 0) { $packageCovered++ }
        }
    }

    if ($packageValid -eq 0) { continue }

    $packagePercent = [math]::Round(100.0 * $packageCovered / $packageValid, 2)
    [void]$builder.AppendLine("| $($package.name) | $packagePercent% | $packageCovered / $packageValid |")
}

[void]$builder.AppendLine()
[void]$builder.AppendLine('計測対象は `Projects/Math` / `Projects/Physics` / `Projects/Engine/src/Core` (`Core/Platform` を除く)。')
[void]$builder.AppendLine('MSVC + OpenCppCoverage が出せるのは行カバレッジ (C0) まで。')
[void]$builder.AppendLine('分岐 (C1) / 条件・MC/DC (C2) は `Tools/Coverage/RunCoverageLLVM.ps1` (clang-cl + llvm-cov) で測る。')

$markdown = $builder.ToString()
$markdown | Out-File -FilePath $OutputPath -Encoding utf8
Write-Host $markdown

# ワークフローの後段 (しきい値判定) が読む出力。
if ($env:GITHUB_OUTPUT) {
    "line-rate-percent=$linePercent" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
