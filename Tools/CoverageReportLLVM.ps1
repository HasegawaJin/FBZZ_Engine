# llvm-cov の JSON エクスポートから Markdown のカバレッジ要約を作る。
#
# WHY 独立した要約が要るか: CoverageReport.ps1 は Cobertura を読む。llvm-cov は Cobertura を
#     出せず、分岐と MC/DC を持つのは JSON と lcov だけ。無理に Cobertura へ変換すると
#     MC/DC の欄が消える (Cobertura にその概念が無い) ので、こちらは JSON を直接読む。
#
# 網羅基準の対応:
#   line     命令網羅 (C0)             … 実行された行
#   branch   分岐 + 条件網羅 (C1 / C2) … clang は && / || の «項ごと» に分岐リージョンを作るため、
#                                        判定単位 (C1) だけでなく条件単位 (C2) まで数えている
#   mcdc     改良条件判定網羅          … 各条件が単独で結果を変える組み合わせを通ったか。
#                                        C2 より厳しく、DO-178C が要求するのはこの基準
#   region   リージョン網羅 (参考)     … 三項演算子や短絡評価で分かれる «式の一部» まで含む

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$JsonPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $JsonPath)) {
    throw "llvm-cov の JSON が見つかりません: $JsonPath"
}

$repoRoot = (Split-Path -Parent $PSScriptRoot).TrimEnd('\')
$export = Get-Content -LiteralPath $JsonPath -Raw | ConvertFrom-Json
$data = $export.data[0]
$totals = $data.totals

function Get-Percent {
    param($Metric)
    if (-not $Metric -or $Metric.count -eq 0) { return $null }
    return [math]::Round(100.0 * $Metric.covered / $Metric.count, 2)
}

function Format-Row {
    param([string]$Label, [string]$Criterion, $Metric)
    $percent = Get-Percent -Metric $Metric
    if ($null -eq $percent) {
        return "| $Label | $Criterion | — | 計測対象なし |"
    }
    return "| $Label | $Criterion | $percent% | $($Metric.covered) / $($Metric.count) |"
}

function ConvertTo-Relative {
    param([string]$Path)
    $normalized = ($Path -replace '/', '\')
    if ($normalized.StartsWith("$repoRoot\", [StringComparison]::OrdinalIgnoreCase)) {
        return $normalized.Substring($repoRoot.Length + 1) -replace '\\', '/'
    }
    return $normalized -replace '\\', '/'
}

$branchPercent = Get-Percent -Metric $totals.branches
$linePercent = Get-Percent -Metric $totals.lines

# 見出しは分岐で付ける。行カバレッジは RunCoverage.ps1 側が既に出しており、
# こちらを走らせる理由は «分岐まで見たいから» なので、目に入る数字をそちらへ寄せる。
$headline = if ($null -ne $branchPercent) { $branchPercent } else { $linePercent }
$badge = if ($headline -ge 90) { '🟢' } elseif ($headline -ge 75) { '🟡' } else { '🔴' }

$builder = [System.Text.StringBuilder]::new()
[void]$builder.AppendLine("## $badge 分岐カバレッジ $headline%")
[void]$builder.AppendLine()
[void]$builder.AppendLine('| 指標 | 網羅基準 | カバレッジ | 到達 / 全体 |')
[void]$builder.AppendLine('|---|---|---:|---:|')
[void]$builder.AppendLine((Format-Row -Label 'line'   -Criterion 'C0 命令網羅'          -Metric $totals.lines))
[void]$builder.AppendLine((Format-Row -Label 'branch' -Criterion 'C1 分岐 + C2 条件網羅' -Metric $totals.branches))
if ($totals.PSObject.Properties.Name -contains 'mcdc') {
    [void]$builder.AppendLine((Format-Row -Label 'mcdc' -Criterion 'MC/DC (C2 を包含)' -Metric $totals.mcdc))
}
[void]$builder.AppendLine((Format-Row -Label 'region'   -Criterion 'リージョン網羅 (参考)' -Metric $totals.regions))
[void]$builder.AppendLine((Format-Row -Label 'function' -Criterion '関数網羅 (参考)'       -Metric $totals.functions))
[void]$builder.AppendLine()

# モジュール別。分母を OpenCppCoverage 側と揃えているので、行の数字は summary.md と突き合わせられる。
$modules = [ordered]@{
    'Projects/Math'            = 'Math'
    'Projects/Physics'         = 'Physics'
    'Projects/Engine/src/Core' = 'Engine Core'
}
[void]$builder.AppendLine('| モジュール | 行 (C0) | 分岐 (C1/C2) |')
[void]$builder.AppendLine('|---|---:|---:|')
foreach ($prefix in $modules.Keys) {
    $files = @($data.files | Where-Object { (ConvertTo-Relative -Path $_.filename).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
    if ($files.Count -eq 0) { continue }

    # WHY 手で足すか: Measure-Object へスクリプトブロックを渡せるのは PowerShell 7 以降。
    #     VS Code のタスクは Windows PowerShell 5.1 を起動するため、そこでは動かない。
    $lineCount = 0; $lineCovered = 0; $branchCount = 0; $branchCovered = 0
    foreach ($file in $files) {
        $lineCount     += $file.summary.lines.count
        $lineCovered   += $file.summary.lines.covered
        $branchCount   += $file.summary.branches.count
        $branchCovered += $file.summary.branches.covered
    }

    $line = if ($lineCount -gt 0) { "$([math]::Round(100.0 * $lineCovered / $lineCount, 2))% ($lineCovered / $lineCount)" } else { '—' }
    $branch = if ($branchCount -gt 0) { "$([math]::Round(100.0 * $branchCovered / $branchCount, 2))% ($branchCovered / $branchCount)" } else { '—' }
    [void]$builder.AppendLine("| $($modules[$prefix]) | $line | $branch |")
}
[void]$builder.AppendLine()

# WHY 率だけで終えないか: 「分岐 62%」は次の行動に繋がらない。«分岐の未到達本数が多い順» に
#     並べると、1 本のテストで最も多くの分岐を潰せるファイルがそのまま先頭に来る。
$worst = @($data.files |
    Where-Object { $_.summary.branches.count -gt 0 -and $_.summary.branches.count -ne $_.summary.branches.covered } |
    Sort-Object -Property { $_.summary.branches.count - $_.summary.branches.covered } -Descending |
    Select-Object -First 10)
if ($worst.Count -gt 0) {
    [void]$builder.AppendLine('### 未到達の分岐が多いファイル')
    [void]$builder.AppendLine()
    [void]$builder.AppendLine('| ファイル | 未到達の分岐 | 分岐カバレッジ |')
    [void]$builder.AppendLine('|---|---:|---:|')
    foreach ($file in $worst) {
        $missing = $file.summary.branches.count - $file.summary.branches.covered
        $percent = [math]::Round(100.0 * $file.summary.branches.covered / $file.summary.branches.count, 2)
        [void]$builder.AppendLine("| $(ConvertTo-Relative -Path $file.filename) | $missing | $percent% |")
    }
    [void]$builder.AppendLine()
}

[void]$builder.AppendLine('計測対象は `Projects/Math` / `Projects/Physics` / `Projects/Engine/src/Core`。')
[void]$builder.AppendLine('clang-cl + llvm-cov による計測 (`Tools/RunCoverageLLVM.ps1`)。')

$markdown = $builder.ToString()
$markdown | Out-File -FilePath $OutputPath -Encoding utf8
Write-Host $markdown

if ($env:GITHUB_OUTPUT) {
    "line-rate-percent=$linePercent"     | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "branch-rate-percent=$branchPercent" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
