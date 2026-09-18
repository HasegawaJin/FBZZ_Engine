# OpenCppCoverage が出した Cobertura XML の中のパスを、リポジトリ相対へ書き換える。
#
# WHY 要るか: OpenCppCoverage は <source> にドライブ文字だけ (`C:`) を置き、
#     <class filename> を «ドライブを外した絶対パス» (`Users\jinhs\...\Vector3.cpp`) にする。
#     これは計測した開発機でしか解けない形なので、次の 3 つが同時に起きる。
#       - VS Code の Coverage Gutters が対応するソースを見つけられず、エディターに何も出ない
#       - ReportGenerator がソースを読めず、行が空欄の HTML になる
#       - CI (ランナーのユーザー名は runneradmin) の XML と手元の XML が別物になり、
#         ReportGenerator の履歴が «同じファイル» として繋がらない
#     ここで «<source> = リポジトリルート / filename = リポジトリ相対» へ正規化して、
#     どのマシンで測った XML でも同じ意味になる状態にする。
#
# あわせて <package name> も «テスト exe の絶対パス» から実行ファイル名だけに縮める。
# CoverageReport.ps1 がこれを Markdown 表のモジュール名として出すため、
# そのままだと表の 1 列が開発機の絶対パスで埋まる。
#
#   .\Tools\Coverage\NormalizeCoverageXml.ps1 -CoberturaPath Artifacts\Coverage\coverage.xml
#
# 同じ XML へ二度掛けても結果は変わらない (書き換え済みの相対パスはそのまま通る)。

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CoberturaPath,
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $CoberturaPath)) {
    throw "Cobertura XML が見つかりません: $CoberturaPath"
}
if (-not $RepositoryRoot) {
    $RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
}
$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd('\', '/')

[xml]$report = Get-Content -LiteralPath $CoberturaPath -Raw

# 正規化前の <source>。filename と繋いで初めて絶対パスになる。
$sourcePrefix = ''
$sourceNodes = $report.SelectNodes('/coverage/sources/source')
if ($sourceNodes.Count -gt 0) {
    $sourcePrefix = $sourceNodes[0].InnerText.TrimEnd('\', '/')
}

function ConvertTo-RepositoryRelativePath {
    param([string]$Filename)

    # 既に相対 (二度掛け) ならそのまま返す。
    $isAbsolute = $Filename -match '^[A-Za-z]:' -or $Filename.StartsWith('\\')
    if (-not $isAbsolute -and $sourcePrefix) {
        $candidate = "$sourcePrefix\$($Filename.TrimStart('\', '/'))"
    }
    else {
        $candidate = $Filename
    }
    $candidate = $candidate -replace '/', '\'

    # ドライブ文字だけの <source> と繋いだ直後は `C:\Users\...` になっている。
    # Resolve-Path は «存在しないファイル» で失敗するため使わない (削除済みソースでも通す)。
    $full = [System.IO.Path]::GetFullPath($candidate)
    if ($full.StartsWith("$RepositoryRoot\", [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($RepositoryRoot.Length + 1) -replace '\\', '/'
    }

    # リポジトリ外のソース (SDK ヘッダー等) は絶対パスのまま残す。
    return $full -replace '\\', '/'
}

$rewritten = 0
$outside = 0
foreach ($class in $report.SelectNodes('//class[@filename]')) {
    $relative = ConvertTo-RepositoryRelativePath -Filename $class.GetAttribute('filename')
    if ($relative -match '^[A-Za-z]:') { $outside++ } else { $rewritten++ }
    $class.SetAttribute('filename', $relative)
}

foreach ($package in $report.SelectNodes('//package[@name]')) {
    $name = $package.GetAttribute('name')
    if ($name -match '[\\/]') {
        $package.SetAttribute('name', (Split-Path -Leaf ($name -replace '/', '\')))
    }
}

# <sources> はリポジトリルート 1 件に置き換える。ReportGenerator はここを起点に
# filename を解決してソースを読み込む。
$sources = $report.SelectSingleNode('/coverage/sources')
if (-not $sources) {
    $sources = $report.CreateElement('sources')
    [void]$report.DocumentElement.PrependChild($sources)
}
$sources.RemoveAll()
$source = $report.CreateElement('source')
$source.InnerText = ($RepositoryRoot -replace '\\', '/')
[void]$sources.AppendChild($source)

$report.Save((Resolve-Path -LiteralPath $CoberturaPath).Path)

Write-Host "[Coverage] パスを正規化: リポジトリ内 $rewritten 件 / リポジトリ外 $outside 件 ($CoberturaPath)"
