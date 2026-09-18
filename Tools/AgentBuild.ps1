# FBZZ Engine
# AgentBuild.ps1 | Tools
# AI エージェントがエンジン C++ を検証するための入口。出力を «file:line の 1 行» に畳んで返す。
#
# WHY 別の入口か: VcBuild.ps1 は人が VS Code のタスクから叩き、problemMatcher が生の MSBuild 出力を読む。
#     AI は数千行のログを文脈へ入れると肝心のエラーを見失うため、要約と終了コードの契約が要る。
#     VS 環境の作り方は VsEnvironment.ps1 を共有するので «VS の場所を知るのは 1 か所» は崩れない。
#
# 使い方:
#   AgentBuild.ps1 check <file...> [-Preset <p>]      変更したファイルだけコンパイル (リンクしない)
#   AgentBuild.ps1 build <target...> [-Preset <p>]    ターゲットをビルド
#   AgentBuild.ps1 test [-Filter <regex>] [-Preset <p>] ctest を回す
#   AgentBuild.ps1 configure [-Preset <p>]            CMake を作り直す (GLOB に新規ファイルを載せる)
#
# 終了コード: 0 = 成功 / 1 = コードのエラー (コンパイル・リンク・テスト失敗) / 2 = 環境の失敗
# 設計: Docs/design/ai-verification-loop.md

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('check', 'build', 'test', 'configure')]
    [string] $Verb,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]] $Items = @(),

    # 省略時は development → debug → release の順で configure 済みのツリーを使う。
    [string] $Preset = '',

    [string] $Filter = '',

    # 警告も 1 行ずつ出す (既定は件数だけ)。
    [switch] $ShowWarnings,

    # 1 回の出力に載せる診断の上限。超えた分は件数だけ出す。
    [int] $MaxDiagnostics = 40
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# WHY 英語へ固定するか: 日本語版の VS はメッセージを CP932 で出し、PowerShell 5.1 の
#     パイプで読むと化ける。診断コード (C2065 等) は言語に依らないが、本文が読めないと直せない。
$env:VSLANG = '1033'
$env:DOTNET_CLI_UI_LANGUAGE = 'en'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# VcBuild.ps1 の対応表と同じ。preset を足したら両方へ足すこと。
$PresetLayout = [ordered]@{
    'development' = @{ BinaryDir = 'build/Development'; Configuration = 'Development' }
    'debug'       = @{ BinaryDir = 'build/Debug';       Configuration = 'Debug' }
    'release'     = @{ BinaryDir = 'build/Release';     Configuration = 'Release' }
}

function Write-Line([string] $text) { [Console]::Out.WriteLine($text) }

function Resolve-Preset {
    if (-not [string]::IsNullOrWhiteSpace($Preset)) {
        $key = $Preset.ToLowerInvariant()
        if (-not $PresetLayout.Contains($key)) {
            Write-Line "RESULT env-error unknown preset '$Preset' (known: $($PresetLayout.Keys -join ', '))"
            exit 2
        }
        return $key
    }
    foreach ($key in $PresetLayout.Keys) {
        if (Test-Path -LiteralPath (Join-Path $RepositoryRoot "$($PresetLayout[$key].BinaryDir)/CMakeCache.txt")) {
            return $key
        }
    }
    # どのツリーも無ければ development を作る (日常のエディタービルドと同じ構成)。
    return 'development'
}

$PresetKey = Resolve-Preset
$BinaryDirectory = Join-Path $RepositoryRoot $PresetLayout[$PresetKey].BinaryDir
$Configuration = $PresetLayout[$PresetKey].Configuration

$AgentDirectory = Join-Path $RepositoryRoot 'build/agent'
New-Item -ItemType Directory -Force -Path $AgentDirectory | Out-Null
# WHY 実行ごとに別ファイルか: 並列のエージェントや開いたままのビューアーが同じログを掴むと、
#     2 本目が Set-Content で落ちて何もコンパイルしない。最新の 1 本は last-<verb>.txt が指す。
$LogPath = Join-Path $AgentDirectory ("{0}-{1:yyyyMMdd-HHmmss}-{2}.log" -f $Verb, (Get-Date), $PID)
Set-Content -LiteralPath (Join-Path $AgentDirectory "last-$Verb.txt") -Value $LogPath -Encoding UTF8 -ErrorAction SilentlyContinue
Set-Content -LiteralPath $LogPath -Value "[AgentBuild] $Verb preset=$PresetKey $(Get-Date -Format o)" -Encoding UTF8

. (Join-Path $PSScriptRoot 'VsEnvironment.ps1')
try {
    Import-VisualStudioEnvironment
}
catch {
    Write-Line "RESULT env-error $($_.Exception.Message)"
    exit 2
}
Set-Location -LiteralPath $RepositoryRoot

# --- 実行と診断の収集 -------------------------------------------------------

$script:Errors = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:Seen = New-Object 'System.Collections.Generic.HashSet[string]'
$script:EditorLocked = $false

function ConvertTo-RepoPath([string] $path) {
    $normalized = $path.Trim().Replace('\', '/')
    $root = $RepositoryRoot.Replace('\', '/').TrimEnd('/') + '/'
    if ($normalized.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $normalized.Substring($root.Length)
    }
    return $normalized
}

function Add-Diagnostic([string] $severity, [string] $text) {
    if (-not $script:Seen.Add("$severity|$text")) { return }
    if ($severity -eq 'ERROR') { $script:Errors.Add($text) } else { $script:Warnings.Add($text) }
}

# MSVC / MSBuild / CMake / gtest の 1 行を分類する。対象外の行は捨てる。
$script:CompiledNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

function Read-DiagnosticLine([string] $line) {
    # 末尾の " [C:\...\X.vcxproj]" は同じ診断がプロジェクトごとに重複して出る原因なので落とす。
    $text = $line -replace '\s+\[[^\]]+\.vcxproj\]\s*$', ''

    # /v:m の MSBuild は cl.exe に渡した TU 名を 1 行ずつ出す。check が «何もコンパイルせず成功» しないための照合に使う。
    if ($text -match '^\s+(?<name>[\w.\-]+\.(cpp|cxx|cc|c))\s*$') {
        [void]$script:CompiledNames.Add($Matches.name)
        return
    }

    if ($text -match '^\s*(?<file>[A-Za-z]:[^()]+?)\((?<line>\d+)(,(?<col>\d+))?\)\s*:\s*(?<sev>fatal error|error|warning)\s+(?<code>[A-Z]+\d+)\s*:\s*(?<msg>.*)$') {
        $location = "$(ConvertTo-RepoPath $Matches.file):$($Matches.line)"
        if ($Matches.col) { $location += ":$($Matches.col)" }
        $severity = if ($Matches.sev -eq 'warning') { 'WARN' } else { 'ERROR' }
        Add-Diagnostic $severity "$location $($Matches.code) $($Matches.msg.Trim())"
        return
    }
    # gtest の失敗位置: "C:\...\FooTests.cpp(42): error: Expected equality ..."
    if ($text -match '^\s*(?<file>[A-Za-z]:[^()]+?)\((?<line>\d+)\)\s*:\s*error:\s*(?<msg>.*)$') {
        Add-Diagnostic 'ERROR' "$(ConvertTo-RepoPath $Matches.file):$($Matches.line) TEST $($Matches.msg.Trim())"
        return
    }
    # リンカー: "Foo.obj : error LNK2019: ..." / "LINK : fatal error LNK1168: ..."
    if ($text -match '^\s*(?<obj>[^:]+?)\s*:\s*(?<sev>fatal error|error|warning)\s+(?<code>LNK\d+)\s*:\s*(?<msg>.*)$') {
        if ($Matches.code -eq 'LNK1168') { $script:EditorLocked = $true }
        $severity = if ($Matches.sev -eq 'warning') { 'WARN' } else { 'ERROR' }
        Add-Diagnostic $severity "$([System.IO.Path]::GetFileName($Matches.obj.Trim())) $($Matches.code) $($Matches.msg.Trim())"
        return
    }
    if ($text -match '^\s*(?<file>[^:]+)\s*:\s*(fatal error|error)\s+(?<code>(C|D|MSB)\d+)\s*:\s*(?<msg>.*)$') {
        Add-Diagnostic 'ERROR' "$(ConvertTo-RepoPath $Matches.file) $($Matches.code) $($Matches.msg.Trim())"
        return
    }
    if ($text -match '^CMake Error at (?<where>[^:]+):(?<line>\d+)') {
        Add-Diagnostic 'ERROR' "$(ConvertTo-RepoPath $Matches.where):$($Matches.line) CMAKE $text"
        return
    }
    if ($text -match '^\[\s+FAILED\s+\]\s+(?<name>\S+\.\S+)') {
        Add-Diagnostic 'ERROR' "gtest FAILED $($Matches.name)"
        return
    }
}

# 外部コマンドを回し、全行をログへ、診断行を分類器へ流す。終了コードを返す。
function Invoke-Logged([string] $exe, [string[]] $arguments) {
    Add-Content -LiteralPath $LogPath -Value "> $exe $($arguments -join ' ')" -Encoding UTF8
    $writer = New-Object System.IO.StreamWriter($LogPath, $true, (New-Object System.Text.UTF8Encoding($false)))
    # WHY: PowerShell 5.1 は Stop のまま native の stderr を 2>&1 で拾うと、最初の 1 行で例外にする。
    $ErrorActionPreference = 'Continue'
    try {
        & $exe @arguments 2>&1 | ForEach-Object {
            $line = "$_"
            $writer.WriteLine($line)
            Read-DiagnosticLine $line
        }
    }
    finally {
        $writer.Dispose()
    }
    return $LASTEXITCODE
}

function Invoke-Configure {
    Write-Line "[AgentBuild] configure preset=$PresetKey"
    return (Invoke-Logged 'cmake' @('--preset', $PresetKey))
}

function Write-Summary([int] $exitCode, [string] $extra = '') {
    $shown = 0
    foreach ($entry in $script:Errors) {
        if ($shown -ge $MaxDiagnostics) { break }
        Write-Line "ERROR $entry"
        $shown++
    }
    if ($script:Errors.Count -gt $shown) { Write-Line "... $($script:Errors.Count - $shown) more errors (see log)" }
    if ($ShowWarnings) {
        $shownWarnings = 0
        foreach ($entry in $script:Warnings) {
            if ($shownWarnings -ge $MaxDiagnostics) { break }
            Write-Line "WARN $entry"
            $shownWarnings++
        }
    }
    if ($script:EditorLocked) {
        Write-Line 'HINT LNK1168: 起動中の FBZZEditor / テスト exe が出力を掴んでいる。check (コンパイルのみ) を使うか、ユーザーに終了を依頼する'
    }
    $status = if ($exitCode -eq 0 -and $script:Errors.Count -eq 0) { 'ok' } else { 'failed' }
    Write-Line "RESULT $status errors=$($script:Errors.Count) warnings=$($script:Warnings.Count) preset=$PresetKey $extra log=$(ConvertTo-RepoPath $LogPath)"
}

# --- check: ファイル → 所有 .vcxproj → ClCompile ---------------------------

function Get-ProjectIndex {
    $index = @{}
    $projects = Get-ChildItem -LiteralPath $BinaryDirectory -Recurse -Filter '*.vcxproj' -File |
        Where-Object { $_.BaseName -notin @('ALL_BUILD', 'ZERO_CHECK', 'INSTALL', 'RUN_TESTS', 'PACKAGE') }
    foreach ($project in $projects) {
        $content = [System.IO.File]::ReadAllText($project.FullName)
        foreach ($match in [regex]::Matches($content, '<ClCompile Include="([^"]+)"')) {
            $key = $match.Groups[1].Value.Replace('/', '\').ToLowerInvariant()
            if (-not $index.ContainsKey($key)) { $index[$key] = $project.FullName }
        }
    }
    return $index
}

# ヘッダーを include している .cpp を探す。同名 .cpp を優先し、最大 3 本。
function Find-Includers([string] $headerPath) {
    $relative = ConvertTo-RepoPath $headerPath
    $specs = New-Object System.Collections.Generic.List[string]
    if ($relative -match '/include/(?<spec>.+)$') { $specs.Add($Matches.spec) }
    if ($relative -match '/src/(?<spec>.+)$') { $specs.Add($Matches.spec) }
    $specs.Add([System.IO.Path]::GetFileName($relative))

    $sameName = [System.IO.Path]::ChangeExtension($headerPath, '.cpp')
    $result = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $sameName) { $result.Add((Resolve-Path -LiteralPath $sameName).Path) }

    $pattern = '#\s*include\s*[<"]([^<>"]*/)?(' + (($specs | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')[>"]'
    $candidates = Get-ChildItem -LiteralPath (Join-Path $RepositoryRoot 'Projects') -Recurse -Filter '*.cpp' -File |
        Where-Object { $_.FullName -notmatch '\\node_modules\\|\\dist\\|\\out\\' }
    foreach ($candidate in $candidates) {
        if ($result.Count -ge 3) { break }
        if ($result -contains $candidate.FullName) { continue }
        if (Select-String -LiteralPath $candidate.FullName -Pattern $pattern -Quiet) { $result.Add($candidate.FullName) }
    }
    return , $result
}

function Invoke-Check {
    if ($Items.Count -eq 0) {
        Write-Line 'RESULT env-error check にはファイルが必要です'
        exit 2
    }
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'CMakeCache.txt'))) {
        if ((Invoke-Configure) -ne 0) { Write-Summary 1 'stage=configure'; exit 2 }
    }

    $units = New-Object System.Collections.Generic.List[string]
    foreach ($item in $Items) {
        $full = if ([System.IO.Path]::IsPathRooted($item)) { $item } else { Join-Path $RepositoryRoot $item }
        if (-not (Test-Path -LiteralPath $full)) { Write-Line "SKIP $item (not found)"; continue }
        $full = (Resolve-Path -LiteralPath $full).Path
        $extension = [System.IO.Path]::GetExtension($full).ToLowerInvariant()
        if ($extension -in @('.cpp', '.cc', '.cxx', '.c')) {
            $units.Add($full)
        }
        elseif ($extension -in @('.hpp', '.h', '.inl')) {
            $includers = Find-Includers $full
            if ($includers.Count -eq 0) { Write-Line "SKIP $item (no .cpp includes it)"; continue }
            foreach ($unit in $includers) {
                if (-not $units.Contains($unit)) { $units.Add($unit) }
            }
            Write-Line "[AgentBuild] $(ConvertTo-RepoPath $full) -> $(( $includers | ForEach-Object { ConvertTo-RepoPath $_ }) -join ', ')"
        }
        else {
            Write-Line "SKIP $item (not a C++ source)"
        }
    }
    if ($units.Count -eq 0) { Write-Summary 0 'units=0'; exit 0 }

    $index = Get-ProjectIndex
    $missing = @($units | Where-Object { -not $index.ContainsKey($_.ToLowerInvariant()) })
    if ($missing.Count -gt 0) {
        # GLOB_RECURSE CONFIGURE_DEPENDS は configure し直すまで新規ファイルを知らない。
        Write-Line "[AgentBuild] $($missing.Count) file(s) not in any project; re-configuring"
        if ((Invoke-Configure) -ne 0) { Write-Summary 1 'stage=configure'; exit 1 }
        $index = Get-ProjectIndex
    }

    $byProject = @{}
    foreach ($unit in $units) {
        $key = $unit.ToLowerInvariant()
        if (-not $index.ContainsKey($key)) { Write-Line "SKIP $(ConvertTo-RepoPath $unit) (not compiled by any target)"; continue }
        $project = $index[$key]
        if (-not $byProject.ContainsKey($project)) { $byProject[$project] = New-Object System.Collections.Generic.List[string] }
        $byProject[$project].Add($unit)
    }

    $exitCode = 0
    foreach ($project in $byProject.Keys) {
        $selected = New-Object System.Collections.Generic.List[string]
        $selected.AddRange($byProject[$project])
        # PCH がまだ無いツリーでは、先に PCH 生成用の TU も選ばないと C1083 で止まる。
        $projectName = [System.IO.Path]::GetFileNameWithoutExtension($project)
        $pchOutput = Join-Path (Split-Path -Parent $project) "$projectName.dir/$Configuration/cmake_pch.pch"
        if (-not (Test-Path -LiteralPath $pchOutput)) {
            $content = [System.IO.File]::ReadAllText($project)
            foreach ($match in [regex]::Matches($content, '<ClCompile Include="([^"]+cmake_pch\.cxx)"')) {
                $selected.Insert(0, $match.Groups[1].Value)
            }
        }
        Write-Line "[AgentBuild] compile $projectName ($($byProject[$project].Count) file(s)) config=$Configuration"
        # WHY 応答ファイルか: コマンドラインの /p: は ';' をプロパティの区切りとして読み (MSB1006)、
        #     %3B も SelectedFiles では解かれず «1 本の存在しないパス» になって何もコンパイルされない。
        #     応答ファイル内の引用符付きの値だけが ';' 区切りのリストとして正しく渡る。
        $responseFile = Join-Path $AgentDirectory "check-$projectName.rsp"
        $responseLines = @(
            "`"$project`"", '/t:ClCompile', "/p:Configuration=$Configuration", '/p:Platform=x64',
            "/p:SelectedFiles=`"$($selected -join ';')`"", '/nologo', '/v:m', '/m:1')
        [System.IO.File]::WriteAllLines($responseFile, $responseLines, (New-Object System.Text.UTF8Encoding($false)))
        $code = Invoke-Logged 'msbuild' @("@$responseFile")
        if ($code -ne 0) { $exitCode = 1 }
        foreach ($unit in $byProject[$project]) {
            if (-not $script:CompiledNames.Contains([System.IO.Path]::GetFileName($unit))) {
                Add-Diagnostic 'ERROR' "$(ConvertTo-RepoPath $unit) NOT-COMPILED MSBuild がこの TU を選ばなかった (unity build / 除外設定 / パス不一致)"
                $exitCode = 1
            }
        }
    }
    Write-Summary $exitCode "units=$($units.Count)"
    if ($exitCode -ne 0 -or $script:Errors.Count -gt 0) { exit 1 }
    exit 0
}

# --- build / test / configure ---------------------------------------------

function Invoke-Build {
    if ($Items.Count -eq 0) {
        Write-Line 'RESULT env-error build にはターゲットが必要です (例: FBZZEditorLauncher)'
        exit 2
    }
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'CMakeCache.txt'))) {
        if ((Invoke-Configure) -ne 0) { Write-Summary 1 'stage=configure'; exit 2 }
    }
    $arguments = New-Object System.Collections.Generic.List[string]
    $arguments.AddRange([string[]]@('--build', $BinaryDirectory, '--config', $Configuration, '--parallel', '1'))
    foreach ($target in $Items) { $arguments.AddRange([string[]]@('--target', $target)) }
    $arguments.AddRange([string[]]@('--', '/nologo', '/v:m'))
    Write-Line "[AgentBuild] build $($Items -join ' ') config=$Configuration"
    $code = Invoke-Logged 'cmake' $arguments.ToArray()
    Write-Summary $code "targets=$($Items -join ',')"
    if ($code -ne 0 -or $script:Errors.Count -gt 0) { exit 1 }
    exit 0
}

function Invoke-Test {
    $arguments = New-Object System.Collections.Generic.List[string]
    $arguments.AddRange([string[]]@('--test-dir', $BinaryDirectory, '-C', $Configuration, '--output-on-failure', '--timeout', '60', '--no-tests=error'))
    if (-not [string]::IsNullOrWhiteSpace($Filter)) { $arguments.AddRange([string[]]@('-R', $Filter)) }
    Write-Line "[AgentBuild] ctest config=$Configuration filter=$Filter"
    $code = Invoke-Logged 'ctest' $arguments.ToArray()
    Write-Summary $code
    if ($code -ne 0 -or $script:Errors.Count -gt 0) { exit 1 }
    exit 0
}

switch ($Verb) {
    'check'     { Invoke-Check }
    'build'     { Invoke-Build }
    'test'      { Invoke-Test }
    'configure' {
        $code = Invoke-Configure
        Write-Summary $code
        if ($code -ne 0) { exit 1 }
        exit 0
    }
}
