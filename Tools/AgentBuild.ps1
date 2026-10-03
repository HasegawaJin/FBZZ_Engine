# @file    AgentBuild.ps1
# @brief   AI エージェントがエンジン C++ を検証する入口。出力を file:line の 1 行に畳む。
# @author  Hasegawa Jin
# @date    2026-09-17
#
# @note 人用の VcBuild.ps1 とは別。AI は生の MSBuild ログを文脈に入れるとエラーを見失うため要約と終了コードを返す。
# @note 終了コード: 0 = 成功 / 1 = コードのエラー (コンパイル・リンク・テスト失敗) / 2 = 環境の失敗・BUSY。
# @note VS 環境の作り方は VsEnvironment.ps1 と共有し、VS の場所を知るのは 1 か所に保つ。
# @see Docs/design/ai-verification-loop.md

# @note check <file...> [-Preset <p>]: 変更したファイルだけコンパイルする (リンクしない)。
# @note build <target...> [-Preset <p>] [-BuildDirectory <dir>]: エンジンまたは構成済み SDK consumer をビルドする。
# @note test [-Filter <regex>] [-Preset <p>]: ctest を回す。
# @note configure [-Preset <p>]: CMake を作り直す (GLOB に新規ファイルを載せる)。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('check', 'build', 'test', 'configure')]
    [string] $Verb,

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]] $Items = @(),

    # @note 省略時は development → debug → release の順で configure 済みのツリーを使う。
    [string] $Preset = '',

    # @note build のみ。リポジトリ内の構成済み Visual Studio consumer のビルド先を指定する。
    [string] $BuildDirectory = '',

    [string] $Filter = '',

    # @note 既定は警告の件数だけを出す。
    [switch] $ShowWarnings,

    # @note 超えた分の診断は件数だけ出す。
    [int] $MaxDiagnostics = 40
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepositoryRoot = Split-Path -Parent $PSScriptRoot

# @note 日本語版 VS の CP932 出力は PowerShell 5.1 のパイプで化けるため、診断本文を英語へ固定する。
$env:VSLANG = '1033'
$env:DOTNET_CLI_UI_LANGUAGE = 'en'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# @note VcBuild.ps1 の対応表と同じ。preset を足したら両方へ足す。
$PresetLayout = [ordered]@{
    'development' = @{ BinaryDir = 'build/Development'; Configuration = 'Development' }
    'debug'       = @{ BinaryDir = 'build/Debug';       Configuration = 'Debug' }
    'release'     = @{ BinaryDir = 'build/Release';     Configuration = 'Release' }
}

function Write-Line([string] $text) { [Console]::Out.WriteLine($text) }

# @brief リポジトリ内の実在ディレクトリを絶対解決する。リンク経由の外部参照も拒否する。
function Resolve-RepositoryDirectory([string] $path, [ref] $reason) {
    try {
        $candidate = if ([System.IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $RepositoryRoot $path }
        $resolved = [System.IO.Path]::GetFullPath($candidate).TrimEnd([char[]]'\/')
        $root = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([char[]]'\/')
        if (-not ($resolved.Equals($root, [System.StringComparison]::OrdinalIgnoreCase) -or
            $resolved.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase))) {
            $reason.Value = "directory is outside repository: $resolved"
            return ''
        }
        if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
            $reason.Value = "directory is missing: $resolved"
            return ''
        }
        $directory = Get-Item -LiteralPath $resolved -Force
        while ($null -ne $directory) {
            if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                $reason.Value = "linked directory is not supported: $($directory.FullName)"
                return ''
            }
            if ($directory.FullName.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return $resolved }
            $directory = $directory.Parent
        }
        $reason.Value = "directory does not resolve inside repository: $resolved"
    }
    catch { $reason.Value = "invalid directory '$path': $($_.Exception.Message)" }
    return ''
}

# @brief 使う preset を決める。明示された preset が未知なら exit 2。
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
    # @note どのツリーも無ければ日常のエディタービルドと同じ development を作る。
    return 'development'
}

$PresetKey = Resolve-Preset
$BinaryDirectory = Join-Path $RepositoryRoot $PresetLayout[$PresetKey].BinaryDir
$Configuration = $PresetLayout[$PresetKey].Configuration

# @note consumer の指定は既存キャッシュの検査だけで受理し、Engine の自動 configure へ流さない。
$UsesBuildDirectory = $PSBoundParameters.ContainsKey('BuildDirectory')
if ($UsesBuildDirectory) {
    if ($Verb -ne 'build' -or [string]::IsNullOrWhiteSpace($BuildDirectory)) {
        Write-Line 'RESULT env-error -BuildDirectory requires build and a configured directory'
        exit 2
    }
    $directoryReason = ''
    $resolvedBuildDirectory = Resolve-RepositoryDirectory $BuildDirectory ([ref] $directoryReason)
    if ([string]::IsNullOrEmpty($resolvedBuildDirectory)) {
        Write-Line "RESULT env-error $directoryReason"
        exit 2
    }
    $cachePath = Join-Path $resolvedBuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf) -or
        ((Get-Item -LiteralPath $cachePath -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        Write-Line "RESULT env-error configured CMakeCache.txt is missing or linked: $cachePath"
        exit 2
    }
    try { $cacheText = [System.IO.File]::ReadAllText($cachePath) }
    catch { Write-Line "RESULT env-error cannot read CMakeCache.txt: $cachePath"; exit 2 }
    $sourceMatch = [regex]::Match($cacheText, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=([^\r\n]+)\r?$')
    $cacheDirectoryMatch = [regex]::Match($cacheText, '(?m)^CMAKE_CACHEFILE_DIR:INTERNAL=([^\r\n]+)\r?$')
    $generatorMatch = [regex]::Match($cacheText, '(?m)^CMAKE_GENERATOR:INTERNAL=([^\r\n]+)\r?$')
    $configurationMatch = [regex]::Match($cacheText, '(?m)^CMAKE_CONFIGURATION_TYPES:STRING=([^\r\n]+)\r?$')
    if (-not $sourceMatch.Success -or -not $cacheDirectoryMatch.Success -or
        -not $generatorMatch.Success -or -not $configurationMatch.Success) {
        Write-Line "RESULT env-error incomplete Visual Studio CMakeCache.txt: $cachePath"
        exit 2
    }
    $resolvedSourceDirectory = Resolve-RepositoryDirectory $sourceMatch.Groups[1].Value ([ref] $directoryReason)
    if ([string]::IsNullOrEmpty($resolvedSourceDirectory)) {
        Write-Line "RESULT env-error CMAKE_HOME_DIRECTORY $directoryReason"
        exit 2
    }
    $resolvedCacheDirectory = Resolve-RepositoryDirectory $cacheDirectoryMatch.Groups[1].Value ([ref] $directoryReason)
    if ([string]::IsNullOrEmpty($resolvedCacheDirectory) -or
        -not $resolvedCacheDirectory.Equals($resolvedBuildDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Line 'RESULT env-error CMAKE_CACHEFILE_DIR does not match -BuildDirectory'
        exit 2
    }
    if ($generatorMatch.Groups[1].Value -notmatch '^Visual Studio \d+ ') {
        Write-Line 'RESULT env-error -BuildDirectory requires a Visual Studio CMake generator'
        exit 2
    }
    if ($Configuration -notin ($configurationMatch.Groups[1].Value -split ';')) {
        Write-Line "RESULT env-error configuration '$Configuration' is not configured in $resolvedBuildDirectory"
        exit 2
    }
    $BinaryDirectory = $resolvedBuildDirectory
}

$AgentDirectory = Join-Path $RepositoryRoot 'build/agent'
New-Item -ItemType Directory -Force -Path $AgentDirectory | Out-Null
# @note ログは実行ごとに別ファイル。同じログを別プロセスが掴むと Set-Content で落ちるため。
# @note 最新の 1 本は last-<verb>.txt が指す。
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

# @name 実行と診断の収集

$script:Errors = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]
$script:Seen = New-Object 'System.Collections.Generic.HashSet[string]'
$script:EditorLocked = $false
$script:TreeContended = $false

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

$script:CompiledNames = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

# @brief MSVC / MSBuild / CMake / gtest の 1 行を分類する。対象外の行は捨てる。
function Read-DiagnosticLine([string] $line) {
    # @note 末尾の .vcxproj 注記はプロジェクトごとに同じ診断を重複させるので落とす。
    $text = $line -replace '\s+\[[^\]]+\.vcxproj\]\s*$', ''

    # @note 別のビルドと出力を奪い合った印。分類はこの後も通常どおり続ける。
    # @note C1083 は include 欠落と同じ番号なので Permission denied のときだけ拾う。
    if ($text -match 'being used by another process|\b(C1041|MSB3491|MSB3026|MSB3027|LNK1104)\b|C1083.*Permission denied') {
        $script:TreeContended = $true
    }

    # @note /v:m の MSBuild は TU 名を 1 行ずつ出す。check が何もコンパイルせず成功しないための照合に使う。
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
    # @note gtest の失敗位置: "C:\...\FooTests.cpp(42): error: Expected equality ..."
    if ($text -match '^\s*(?<file>[A-Za-z]:[^()]+?)\((?<line>\d+)\)\s*:\s*error:\s*(?<msg>.*)$') {
        Add-Diagnostic 'ERROR' "$(ConvertTo-RepoPath $Matches.file):$($Matches.line) TEST $($Matches.msg.Trim())"
        return
    }
    # @note リンカー: "Foo.obj : error LNK2019: ..." / "LINK : fatal error LNK1168: ..."
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

# @brief 外部コマンドを回し、全行をログへ、診断行を分類器へ流す。
# @return 外部コマンドの終了コード。
function Invoke-Logged([string] $exe, [string[]] $arguments) {
    Add-Content -LiteralPath $LogPath -Value "> $exe $($arguments -join ' ')" -Encoding UTF8
    $writer = New-Object System.IO.StreamWriter($LogPath, $true, (New-Object System.Text.UTF8Encoding($false)))
    # @note PowerShell 5.1 は Stop のまま native の stderr を 2>&1 で拾うと最初の 1 行で例外にする。
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

# @brief 収集した診断と HINT を出し、最後に RESULT 行を 1 行出す。
# @param extra RESULT 行へ足す key=value (stage= / units= / targets=)。
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
    if ($script:TreeContended) {
        Write-Line 'HINT CONTENDED: 別のビルドと出力を奪い合った可能性が高い。コードを直さない。ユーザーに実行中のビルドを確かめてもらい、終わってから同じコマンドを 1 回だけ再実行する'
    }
    $status = if ($exitCode -eq 0 -and $script:Errors.Count -eq 0) { 'ok' } else { 'failed' }
    Write-Line "RESULT $status errors=$($script:Errors.Count) warnings=$($script:Warnings.Count) preset=$PresetKey $extra log=$(ConvertTo-RepoPath $LogPath)"
}

# @name check: ファイル → 所有 .vcxproj → ClCompile

# @brief 生成済み .vcxproj を走査し、小文字化した TU の絶対パス → 所有 .vcxproj の表を作る。
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

# @brief ヘッダーを include している .cpp を Projects/ から探す。
# @return 同名 .cpp を優先した最大 3 本の絶対パス。
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

# @brief 指定ファイル (ヘッダーは include 元の .cpp) を所有プロジェクトごとに ClCompile だけ回す。
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
        # @note GLOB_RECURSE CONFIGURE_DEPENDS は configure し直すまで新規ファイルを知らない。
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
        # @note PCH がまだ無いツリーでは、PCH 生成用の TU も先頭に選ばないと C1083 で止まる。
        $projectName = [System.IO.Path]::GetFileNameWithoutExtension($project)
        $pchOutput = Join-Path (Split-Path -Parent $project) "$projectName.dir/$Configuration/cmake_pch.pch"
        if (-not (Test-Path -LiteralPath $pchOutput)) {
            $content = [System.IO.File]::ReadAllText($project)
            foreach ($match in [regex]::Matches($content, '<ClCompile Include="([^"]+cmake_pch\.cxx)"')) {
                $selected.Insert(0, $match.Groups[1].Value)
            }
        }
        Write-Line "[AgentBuild] compile $projectName ($($byProject[$project].Count) file(s)) config=$Configuration"
        # @note SelectedFiles は応答ファイル経由で渡す。コマンドラインの /p: は ';' をプロパティ区切りと読み (MSB1006)、
        # @note %3B も解かれず 1 本の存在しないパスになる。応答ファイル内の引用符付きの値だけが ';' 区切りで渡る。
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

# @name build / test / configure

# @brief Items のターゲットを cmake --build でビルドする (リンクまで行う)。
function Invoke-Build {
    if ($Items.Count -eq 0) {
        Write-Line 'RESULT env-error build にはターゲットが必要です (例: FBZZEditorLauncher)'
        exit 2
    }
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory 'CMakeCache.txt'))) {
        if ($UsesBuildDirectory) {
            Write-Line 'RESULT env-error consumer CMakeCache.txt disappeared; configure it before building'
            exit 2
        }
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

# @name 他のビルドとの重なり

# @brief 走っている cl / link / MSBuild / cmake / ctest を返す。1 本でもあれば BUSY で始めない。
# @note 同じツリーの奪い合いは C1041 / C1083 / LNK1104 / MSB3491 とコードのエラーの顔で落ち、AI が正しいコードを壊す。
# @note 別ツリーでも断る。cl.exe は 1 本 1〜2GB 使い、並列の 2 本でメモリを使い切る。
# @note 上書きの引数は意図して持たない。逃げ道があると AI は BUSY のたびにそれを使う。
function Get-ConcurrentBuild {
    $names = "Name='cl.exe' OR Name='link.exe' OR Name='MSBuild.exe' OR Name='cmake.exe' OR Name='ctest.exe'"
    $found = @()
    foreach ($process in @(Get-CimInstance Win32_Process -Filter $names -ErrorAction SilentlyContinue)) {
        $commandLine = [string]$process.CommandLine
        # @note /nodemode: はノード再利用で待機しているだけのワーカー。実ビルド中は親の MSBuild / cl.exe が別にいる。
        if ($process.Name -eq 'MSBuild.exe' -and $commandLine -match '/nodemode:') { continue }
        $found += $process
    }
    return $found
}

$running = @(Get-ConcurrentBuild)
if ($running.Count -gt 0) {
    foreach ($process in $running) {
        $commandLine = [string]$process.CommandLine
        if ($commandLine.Length -gt 160) { $commandLine = $commandLine.Substring(0, 160) + '...' }
        Write-Line "BUSY pid=$($process.ProcessId) $($process.Name) $commandLine"
    }
    Write-Line 'HINT BUSY: 別のビルドが走っている (人のタスク / GameHub + SDK / 別のエージェント)。止めない・消さない。終わるのを待つか、ユーザーに確かめる'
    Write-Line "RESULT busy running=$($running.Count) preset=$PresetKey log=$(ConvertTo-RepoPath $LogPath)"
    exit 2
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
