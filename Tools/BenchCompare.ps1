# @file    BenchCompare.ps1
# @brief   基準と候補の FBZZTestBench をビルドして交互に計測し、比較レポートを作る。
# @author  Hasegawa Jin
# @date    2026-09-18
#
# @note 使い方: .\Tools\BenchCompare.ps1 -Baseline <ref> [-Candidate <ref>] [-Rounds 3] [-Title <題名>] [-Publish <名前>]
# @note -Candidate を省くと作業ツリー (未コミットの変更を含む) を候補にする。
# @note 終了コード: 0 = 成功 / 1 = 計測・レポートの失敗 / 2 = 環境・ビルドの失敗 (BUSY を含む)。
# @see Docs/design/benchmark-report.md
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Baseline,

    [string] $Candidate = '',

    # @note 基準 → 候補 を 1 組として何組回すか。交互に回してマシン状態の変化による偏りを打ち消す。
    [int] $Rounds = 3,

    [string] $Title = '',

    # @note 付けると Docs/benchmarks/<日付>-<名前>/ に書き、一覧を作り直す。無ければ build/bench/reports/ に書く。
    [string] $Publish = '',

    [string] $BaselineLabel = 'before',
    [string] $CandidateLabel = 'after',

    # @note 0 は TestBench の既定値を使う。
    [int] $Steps = 0,
    [int] $Warmup = 0,
    [int] $Repeats = 0,
    [int] $Ops = 0
)

Set-StrictMode -Version Latest
# @note Stop にしない。Windows PowerShell 5.1 は Stop のまま外部コマンドの stderr を受けると、git の進捗表示まで例外にして止まる。
# @note 外部コマンドの成否は $LASTEXITCODE で、ファイル操作は個別の -ErrorAction Stop で判定する。
$ErrorActionPreference = 'Continue'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$BenchRoot = Join-Path $RepositoryRoot 'build/bench'
# @note AgentBuild の development preset の出力先。TestBench と、その隣に配置される DLL 一式がここに揃う。
$TestsOutput = 'build/Development/Binaries/Development/Tests'

function Write-Line([string] $text) { [Console]::Out.WriteLine($text) }

function Stop-WithCode([int] $code, [string] $message) {
    Write-Line "RESULT $(if ($code -eq 2) { 'env-error' } else { 'failed' }) $message"
    exit $code
}

function Invoke-Git([string[]] $arguments) {
    $output = & git -C $RepositoryRoot @arguments 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { Stop-WithCode 2 "git $($arguments -join ' '): $output" }
    return ($output -join "`n")
}

# @brief 作業ツリーの TestBench を AgentBuild でビルドし、出力一式を destination へ複製する。
function Build-Bench([string] $sourceRoot, [string] $destination) {
    $agentBuild = Join-Path $sourceRoot 'Tools/AgentBuild.ps1'
    if (-not (Test-Path -LiteralPath $agentBuild)) { Stop-WithCode 2 "AgentBuild.ps1 が無い: $sourceRoot" }
    Write-Line "[BenchCompare] build FBZZTestBench in $sourceRoot"
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $agentBuild build FBZZTestBench -Preset development 2>&1
    $result = $output | Where-Object { "$_" -like 'RESULT *' } | Select-Object -Last 1
    Write-Line "[BenchCompare]   $result"
    if ($LASTEXITCODE -ne 0) { Stop-WithCode 2 "TestBench のビルドに失敗 ($sourceRoot)" }

    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force -ErrorAction Stop }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) -ErrorAction Stop | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot $TestsOutput) -Destination $destination -Recurse -ErrorAction Stop
}

# @brief コミットの TestBench を用意する。キャッシュが無ければ worktree で取り出してビルドする。
# @note worktree は build/bench/src/<hash> に作り、複製し終えたら消す (ソースとビルドで数 GB になるため)。
function Get-CommitBench([string] $ref) {
    $hash = (Invoke-Git @('rev-parse', '--verify', "$ref^{commit}")).Trim()
    $binary = Join-Path $BenchRoot "bin/$hash"
    if (Test-Path -LiteralPath (Join-Path $binary 'FBZZTestBench.exe')) {
        Write-Line "[BenchCompare] cache hit $ref ($($hash.Substring(0, 8)))"
    } else {
        $worktree = Join-Path $BenchRoot "src/$hash"
        if (Test-Path -LiteralPath $worktree) { Invoke-Git @('worktree', 'remove', '--force', $worktree) | Out-Null }
        Write-Line "[BenchCompare] worktree $ref ($($hash.Substring(0, 8))) — 初回は configure から行うため時間がかかる"
        Invoke-Git @('worktree', 'add', '--detach', $worktree, $hash) | Out-Null
        try {
            Build-Bench $worktree $binary
        } finally {
            Invoke-Git @('worktree', 'remove', '--force', $worktree) | Out-Null
        }
    }
    return @{ Exe = Join-Path $binary 'FBZZTestBench.exe'; Commit = $hash; Dirty = $false }
}

# @brief 作業ツリー (未コミットの変更を含む) の TestBench を毎回ビルドし直して用意する。
function Get-WorkingBench {
    $binary = Join-Path $BenchRoot 'bin/working'
    Build-Bench $RepositoryRoot $binary
    $hash = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
    $status = Invoke-Git @('status', '--porcelain', '--untracked-files=no')
    return @{ Exe = Join-Path $binary 'FBZZTestBench.exe'; Commit = $hash; Dirty = -not [string]::IsNullOrWhiteSpace("$status") }
}

# @brief 1 回計測して JSON を書く。
function Invoke-Measure([hashtable] $bench, [string] $label, [string] $jsonPath, [string] $logPath) {
    $arguments = @('--measure', '--json', $jsonPath, '--label', $label, '--commit', $bench.Commit)
    if ($bench.Dirty) { $arguments += '--dirty' }
    if ($Steps -gt 0) { $arguments += @('--steps', "$Steps") }
    if ($Warmup -gt 0) { $arguments += @('--warmup', "$Warmup") }
    if ($Repeats -gt 0) { $arguments += @('--repeats', "$Repeats") }
    if ($Ops -gt 0) { $arguments += @('--ops', "$Ops") }
    & $bench.Exe @arguments *>> $logPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $jsonPath)) {
        Stop-WithCode 1 "$label の計測に失敗 (--measure の JSON 出力に対応していないコミットかもしれない)。log=$logPath"
    }
}

if ($Rounds -lt 1) { Stop-WithCode 2 '-Rounds は 1 以上' }

$baselineBench = Get-CommitBench $Baseline
$candidateBench = if ([string]::IsNullOrWhiteSpace($Candidate)) { Get-WorkingBench } else { Get-CommitBench $Candidate }

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path $BenchRoot "runs/$stamp"
New-Item -ItemType Directory -Force -Path $runDirectory -ErrorAction Stop | Out-Null
$logPath = Join-Path $runDirectory 'measure.log'

for ($round = 1; $round -le $Rounds; $round++) {
    Write-Line "[BenchCompare] round $round/$Rounds"
    Invoke-Measure $baselineBench $BaselineLabel (Join-Path $runDirectory ("{0}-{1}.json" -f $BaselineLabel, $round)) $logPath
    Invoke-Measure $candidateBench $CandidateLabel (Join-Path $runDirectory ("{0}-{1}.json" -f $CandidateLabel, $round)) $logPath
}

$benchmarks = Join-Path $RepositoryRoot 'Docs/benchmarks'
$outDirectory = if ([string]::IsNullOrWhiteSpace($Publish)) {
    Join-Path $BenchRoot "reports/$stamp"
} else {
    Join-Path $benchmarks ("{0}-{1}" -f (Get-Date -Format 'yyyy-MM-dd'), $Publish)
}

$cli = Join-Path $RepositoryRoot 'Projects/DevTools/BenchReport/src/cli.ts'
$reportArguments = @($cli, 'report', $runDirectory, '--out', $outDirectory, '--baseline', $BaselineLabel, '--candidate', $CandidateLabel)
if (-not [string]::IsNullOrWhiteSpace($Title)) { $reportArguments += @('--title', $Title) }
& node @reportArguments
if ($LASTEXITCODE -ne 0) { Stop-WithCode 1 'レポートの生成に失敗' }

if (-not [string]::IsNullOrWhiteSpace($Publish)) {
    & node $cli index $benchmarks
    if ($LASTEXITCODE -ne 0) { Stop-WithCode 1 '一覧の生成に失敗' }
}

Write-Line "RESULT ok report=$outDirectory runs=$runDirectory"
exit 0
