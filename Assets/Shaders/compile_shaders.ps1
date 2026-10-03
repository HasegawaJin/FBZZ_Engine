# @file    compile_shaders.ps1
# @brief   固定 DXC 一式による SM 6.8 コンパイルと生成契約の差分検知。
# @author  Hasegawa Jin
# @date    2026-07-20

[CmdletBinding()]
param(
    # @note 唯一のバックエンド。撤去後も選択の入口を残すのは、次のバックエンドを足すときに
    # @note 呼び出し側のコマンドラインを変えずに済ませるため。
    [ValidateSet("DX12")]
    [string]$Backend = "DX12",

    [switch]$Force,

    [switch]$PlanOnly,

    [switch]$SummaryOnly,

    [string]$CompilerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$shaderRoot = Split-Path -Parent $PSCommandPath
$logPath = Join-Path $shaderRoot "compile_log.txt"
$scriptTimestampUtc = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
$directIncludeCache = @{}

# @note Windows PowerShell 5.1にはPath.GetRelativePathがないため、ルート配下であることを
# @note 確認したうえで文字列として相対化し、CMake/Editorの双方から同じ命名規則を使う。
function Get-ShaderRelativePath {
    param([Parameter(Mandatory = $true)][string]$FullPath)

    $rootPrefix = $shaderRoot.TrimEnd([char[]]@(92, 47)) + [IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Shader path is outside the shader root: $FullPath"
    }

    return $FullPath.Substring($rootPrefix.Length).Replace([char]92, [char]47)
}

# @note includeは記述元の隣を優先し、その後シェーダールートを探索する。
# @note コンパイラーと同じ探索順にすることで、差分判定と実コンパイルの依存解決を一致させる。
function Resolve-ShaderInclude {
    param(
        [Parameter(Mandatory = $true)][string]$IncludingFile,
        [Parameter(Mandatory = $true)][string]$IncludePath
    )

    $normalizedInclude = $IncludePath.Replace("/", [IO.Path]::DirectorySeparatorChar)
    $candidates = @(
        (Join-Path (Split-Path -Parent $IncludingFile) $normalizedInclude),
        (Join-Path $shaderRoot $normalizedInclude)
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    return $null
}

# @note 各ファイルのinclude解析結果をキャッシュし、複数シェーダーが共通hlsliを使っても一度だけ読む。
function Get-DirectShaderIncludes {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $fullPath = [IO.Path]::GetFullPath($FilePath)
    if ($directIncludeCache.ContainsKey($fullPath)) {
        return $directIncludeCache[$fullPath]
    }

    $paths = New-Object System.Collections.Generic.List[string]
    $missing = New-Object System.Collections.Generic.List[string]
    $includePattern = '^\s*#\s*include\s*[<"]([^>"]+)[>"]'

    foreach ($line in [IO.File]::ReadLines($fullPath)) {
        $match = [Text.RegularExpressions.Regex]::Match($line, $includePattern)
        if (-not $match.Success) {
            continue
        }

        $includeName = $match.Groups[1].Value
        $resolved = Resolve-ShaderInclude -IncludingFile $fullPath -IncludePath $includeName
        if ($null -eq $resolved) {
            $missing.Add($includeName)
            continue
        }

        if (-not $paths.Contains($resolved)) {
            $paths.Add($resolved)
        }
    }

    $result = [PSCustomObject]@{
        Paths = $paths.ToArray()
        Missing = $missing.ToArray()
    }
    $directIncludeCache[$fullPath] = $result
    return $result
}

# @note 推移的なincludeグラフをDFSでたどり、出力より新しい依存が一つでもあれば再コンパイルする。
function Get-ShaderDependencyInfo {
    param([Parameter(Mandatory = $true)][string]$SourcePath)

    $visited = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $stack = [Collections.Generic.Stack[string]]::new()
    $missing = New-Object System.Collections.Generic.List[string]
    $files = New-Object System.Collections.Generic.List[string]
    $stack.Push([IO.Path]::GetFullPath($SourcePath))
    $newestUtc = $scriptTimestampUtc
    $newestPath = $PSCommandPath

    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        if (-not $visited.Add($current)) {
            continue
        }

        $files.Add($current)
        $timestampUtc = (Get-Item -LiteralPath $current).LastWriteTimeUtc
        if ($timestampUtc -gt $newestUtc) {
            $newestUtc = $timestampUtc
            $newestPath = $current
        }

        $includes = Get-DirectShaderIncludes -FilePath $current
        foreach ($missingInclude in $includes.Missing) {
            $missing.Add("$(Get-ShaderRelativePath -FullPath $current) -> $missingInclude")
        }
        foreach ($includeFile in $includes.Paths) {
            $stack.Push($includeFile)
        }
    }

    return [PSCustomObject]@{
        Files = $files.ToArray()
        Missing = $missing.ToArray()
        NewestUtc = $newestUtc
        NewestPath = $newestPath
    }
}

# @note 全hlslを再帰収集し、ソースと推移的include内に実在する標準エントリーポイントだけをジョブ化する。
# @note Standalone側の薄いラッパーHLSLはEngine正本をincludeするため、ラッパー本文だけでは誤って未定義扱いになる。
function Get-ShaderJobs {
    $jobs = New-Object System.Collections.Generic.List[object]
    $errors = New-Object System.Collections.Generic.List[object]
    $outputNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $sources = @(Get-ChildItem -LiteralPath $shaderRoot -Recurse -File -Filter "*.hlsl" | Sort-Object FullName)

    foreach ($source in $sources) {
        $relativePath = Get-ShaderRelativePath -FullPath $source.FullName
        $dependencyInfo = Get-ShaderDependencyInfo -SourcePath $source.FullName
        $sourceText = ($dependencyInfo.Files | ForEach-Object { [IO.File]::ReadAllText($_) }) -join "`n"
        $stages = New-Object System.Collections.Generic.List[object]

        if ([Text.RegularExpressions.Regex]::IsMatch($sourceText, '\bVSMain\s*\(')) {
            $stages.Add([PSCustomObject]@{ Name = "VS"; Entry = "VSMain" })
        }
        if ([Text.RegularExpressions.Regex]::IsMatch($sourceText, '\bPSMain\s*\(')) {
            $stages.Add([PSCustomObject]@{ Name = "PS"; Entry = "PSMain" })
        }
        if ([Text.RegularExpressions.Regex]::IsMatch($sourceText, '\bCSMain\s*\(')) {
            $stages.Add([PSCustomObject]@{ Name = "CS"; Entry = "CSMain" })
        }

        if ($stages.Count -eq 0) {
            $errors.Add([PSCustomObject]@{
                Backend = "PRECHECK"
                Stage = "DISCOVERY"
                Entry = "-"
                Source = $relativePath
                Message = "No VSMain, PSMain, or CSMain entry point was found. Include-only files must use .hlsli."
            })
            continue
        }

        $outputBase = ($relativePath -replace '\.hlsl$', '') -replace '[/\\]', '.'

        foreach ($stage in $stages) {
            if ($stage.Name -eq "CS") {
                $outputName = if ($outputBase.EndsWith(".cs", [StringComparison]::OrdinalIgnoreCase)) {
                    "$outputBase.cso"
                } else {
                    "$outputBase.cs.cso"
                }
            } else {
                $outputName = "$outputBase.$($stage.Name.ToLowerInvariant()).cso"
            }

            if (-not $outputNames.Add($outputName)) {
                $errors.Add([PSCustomObject]@{
                    Backend = "PRECHECK"
                    Stage = $stage.Name
                    Entry = $stage.Entry
                    Source = $relativePath
                    Message = "Multiple shaders produce the same output: $outputName"
                })
                continue
            }

            $jobs.Add([PSCustomObject]@{
                SourcePath = $source.FullName
                RelativePath = $relativePath
                Stage = $stage.Name
                Entry = $stage.Entry
                OutputName = $outputName
                Dependencies = $dependencyInfo
            })
        }
    }

    return [PSCustomObject]@{
        SourceCount = $sources.Count
        Jobs = $jobs.ToArray()
        Errors = $errors.ToArray()
    }
}

# @note 生成済み schema 2 manifest の重複しない節だけを読む。
function Get-ShaderSdkSection {
    param([string]$ManifestText, [string]$SectionName)
    $pattern = '(?ms)^[ \t]*\[' + [Text.RegularExpressions.Regex]::Escape($SectionName) + '\][ \t]*\r?\n(?<body>.*?)(?=^[ \t]*\[|\z)'
    $sections = [Text.RegularExpressions.Regex]::Matches($ManifestText, $pattern)
    if ($sections.Count -ne 1) { return $null }
    return $sections[0].Groups["body"].Value
}

# @note 重複した scalar は公開契約として受け入れない。
function Get-ShaderSdkScalar {
    param([string]$SectionText, [string]$Name)
    $pattern = '(?m)^[ \t]*' + [Text.RegularExpressions.Regex]::Escape($Name) + '[ \t]*=[ \t]*(?<value>[^\r\n]*?)[ \t]*\r?$'
    $values = [Text.RegularExpressions.Regex]::Matches($SectionText, $pattern)
    if ($values.Count -ne 1) { return $null }
    return $values[0].Groups["value"].Value
}

# @note 有効な DX12 runtime と同一 fingerprint の検証済み構成だけを Debug、Development、Release の順に選ぶ。
function Get-ShaderSdkCompiler {
    param([string]$SdkRoot)
    if ([string]::IsNullOrWhiteSpace($SdkRoot)) { return $null }
    $manifestPath = Join-Path $SdkRoot "fbzz-sdk.toml"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $null }
    $manifestText = [IO.File]::ReadAllText($manifestPath)
    $sdkSection = Get-ShaderSdkSection -ManifestText $manifestText -SectionName "sdk"
    $runtimeSection = Get-ShaderSdkSection -ManifestText $manifestText -SectionName "runtime"
    if ((Get-ShaderSdkScalar -SectionText $sdkSection -Name "schema") -cne "2" -or
        (Get-ShaderSdkScalar -SectionText $runtimeSection -Name "dx12_enabled") -cne "true") { return $null }
    $fingerprint = Get-ShaderSdkScalar -SectionText $runtimeSection -Name "fingerprint"
    if ($null -eq $fingerprint -or $fingerprint -cnotmatch '^"[0-9a-f]{64}"$') { return $null }
    foreach ($configuration in @("Debug", "Development", "Release")) {
        $section = Get-ShaderSdkSection -ManifestText $manifestText -SectionName "configurations.$configuration"
        if ((Get-ShaderSdkScalar -SectionText $section -Name "validated") -cne "true" -or
            (Get-ShaderSdkScalar -SectionText $section -Name "fingerprint") -cne $fingerprint) { continue }
        $candidate = Join-Path $SdkRoot "tools/$configuration/DXC/dxc.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

# @note 明示 CompilerPath、祖先 source DXC、FBZZ_SDK_ROOT、祖先 SDK、FBZZ_DXC の順に選択し、不備は失敗とする。
# @see https://github.com/microsoft/DirectXShaderCompiler#pre-built-binaries Release binary suite.
function Find-ShaderCompiler {
    param([Parameter(Mandatory = $true)][string]$TargetBackend)
    $selected = $CompilerPath
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $ancestor = Get-Item -LiteralPath $shaderRoot
        while ($null -ne $ancestor) {
            $candidate = Join-Path $ancestor.FullName "ThirdParty/DXC/bin/x64/dxc.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $selected = $candidate; break }
            $ancestor = $ancestor.Parent
        }
    }
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $selected = Get-ShaderSdkCompiler -SdkRoot ([Environment]::GetEnvironmentVariable("FBZZ_SDK_ROOT"))
    }
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $ancestor = Get-Item -LiteralPath $shaderRoot
        while ($null -ne $ancestor) {
            $selected = Get-ShaderSdkCompiler -SdkRoot $ancestor.FullName
            if (-not [string]::IsNullOrWhiteSpace($selected)) { break }
            $ancestor = $ancestor.Parent
        }
    }
    if ([string]::IsNullOrWhiteSpace($selected)) {
        $selected = [Environment]::GetEnvironmentVariable("FBZZ_DXC")
    }
    if ([string]::IsNullOrWhiteSpace($selected)) { throw "Verified DXC suite missing; pass -CompilerPath or FBZZ_DXC." }
    $selected = [IO.Path]::GetFullPath($selected)
    if (-not [IO.Path]::GetFileName($selected).Equals("dxc.exe", [StringComparison]::OrdinalIgnoreCase)) {
        throw "DXC suite executable must be named dxc.exe: $selected"
    }
    $directory = Split-Path -Parent $selected
    $versionPath = $null
    foreach ($searchRoot in @($shaderRoot, $directory)) {
        $ancestor = Get-Item -LiteralPath $searchRoot
        while ($null -ne $ancestor) {
            foreach ($relativePath in @("ThirdParty/DXC/VERSION", "share/fbzz/licenses/DXC/VERSION")) {
                $candidate = Join-Path $ancestor.FullName $relativePath
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $versionPath = $candidate; break }
            }
            if ($null -ne $versionPath) { break }
            $ancestor = $ancestor.Parent
        }
        if ($null -ne $versionPath) { break }
    }
    if ($null -eq $versionPath) { throw "Fixed DXC VERSION contract missing for $selected" }
    $versionText = [IO.File]::ReadAllText($versionPath)
    $hashes = @()
    $hashKeys = @("EXECUTABLE", "COMPILER", "VALIDATOR")
    foreach ($name in @("dxc.exe", "dxcompiler.dll", "dxil.dll")) {
        $file = Join-Path $directory $name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "DXC suite missing: $file" }
        $algorithm = [Security.Cryptography.SHA256]::Create()
        $stream = $null
        try {
            $stream = [IO.File]::OpenRead($file)
            $hash = [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace("-", "").ToLowerInvariant()
        } finally {
            if ($null -ne $stream) { $stream.Dispose() }
            $algorithm.Dispose()
        }
        $key = $hashKeys[$hashes.Count]
        $expected = [Text.RegularExpressions.Regex]::Match($versionText, ('set\(FBZZ_DXC_' + $key + '_SHA256\s+"([0-9a-fA-F]{64})"\)'))
        if (-not $expected.Success) { throw "DXC VERSION lacks $key SHA256: $versionPath" }
        if ($hash -ne $expected.Groups[1].Value.ToLowerInvariant()) {
            throw "DXC suite hash mismatch: $file expected=$($expected.Groups[1].Value) found=$hash. Use the fixed suite from $versionPath."
        }
        $hashes += $hash
    }
    $script:compilerSignature = $hashes -join "|"
    Write-Host "[compile_shaders] compiler=$selected signature=$script:compilerSignature"
    return $selected
}

# @note profile・entry・引数・DXC 一式を成果物の生成契約へ含める。
function Get-ShaderCompileSignature {
    param([Parameter(Mandatory = $true)][object]$Job)
    return "$script:compilerSignature|$($Job.Entry)|$($Job.Stage.ToLowerInvariant())_6_8|FBZZ_BACKEND_DX12=1|HV2021`n-O3 -nologo"
}

function Get-ShaderProfile {
    param(
        [Parameter(Mandatory = $true)][string]$TargetBackend,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $prefix = $Stage.ToLowerInvariant()
    return "${prefix}_6_8"
}

function Test-ShaderJobStale {
    param(
        [Parameter(Mandatory = $true)][object]$Job,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    if ($Force -or -not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        return $true
    }

    $stamp = "$OutputPath.compiler"
    if (-not (Test-Path -LiteralPath $stamp -PathType Leaf) -or
        [IO.File]::ReadAllText($stamp) -ne (Get-ShaderCompileSignature -Job $Job)) { return $true }
    return (Get-Item -LiteralPath $OutputPath).LastWriteTimeUtc -lt $Job.Dependencies.NewestUtc
}

# @note 一時ファイルへのコンパイル成功後だけ本番CSOへ置換し、失敗時に前回の正常成果物を残す。
function Invoke-ShaderJob {
    param(
        [Parameter(Mandatory = $true)][object]$Job,
        [Parameter(Mandatory = $true)][string]$TargetBackend,
        [Parameter(Mandatory = $true)][string]$CompilerPath,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $temporaryPath = "$OutputPath.$([Guid]::NewGuid().ToString('N')).tmp"
    $profile = Get-ShaderProfile -TargetBackend $TargetBackend -Stage $Job.Stage

    $arguments = @(
        "-nologo", "-O3", "-HV", "2021", "-I", $shaderRoot,
        "-D", "FBZZ_BACKEND_DX12=1",
        "-T", $profile, "-E", $Job.Entry,
        "-Fo", $temporaryPath, $Job.SourcePath
    )

    try {
        # @note Windows PowerShell 5.1はネイティブプロセスのstderrもErrorRecordへ変換する。
        # @note コンパイラー警告でStopが発火しないよう、実行中だけContinueへ切り替える。
        $compilerOutput = @()
        $exitCode = -1
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $compilerOutput = @(& $CompilerPath @arguments 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($compilerOutput.Count -gt 0) {
            $compilerOutput | Tee-Object -FilePath $logPath -Append | ForEach-Object { Write-Host $_ }
        }
        $outputWasGenerated = (Test-Path -LiteralPath $temporaryPath -PathType Leaf) -and
            ((Get-Item -LiteralPath $temporaryPath).Length -gt 0)
        if ($exitCode -ne 0 -or -not $outputWasGenerated) {
            throw "source=$($Job.RelativePath) backend=$TargetBackend stage=$($Job.Stage) entry=$($Job.Entry) exitCode=$exitCode"
        }

        Move-Item -LiteralPath $temporaryPath -Destination $OutputPath -Force
        [IO.File]::WriteAllText("$OutputPath.compiler", (Get-ShaderCompileSignature -Job $Job), [Text.UTF8Encoding]::new($false))
    } finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Write-CompileMessage {
    param([Parameter(Mandatory = $true)][string]$Message)

    Write-Host $Message
    if (-not $PlanOnly) {
        Add-Content -LiteralPath $logPath -Value $Message -Encoding UTF8
    }
}

try {
    $selectedCompiler = Find-ShaderCompiler -TargetBackend "DX12"
    $catalog = Get-ShaderJobs
    $failures = New-Object System.Collections.Generic.List[object]
    foreach ($catalogError in $catalog.Errors) {
        $failures.Add($catalogError)
    }
    $missingIncludes = @($catalog.Jobs | ForEach-Object { $_.Dependencies.Missing } | Sort-Object -Unique)
    foreach ($missingInclude in $missingIncludes) {
        $failures.Add([PSCustomObject]@{
            Backend = "PRECHECK"
            Stage = "INCLUDE"
            Entry = "-"
            Source = ($missingInclude -split ' -> ', 2)[0]
            Message = "Unresolved shader include: $missingInclude"
        })
    }

    # @note 文字列がそのまま入ると後段の .Count が StrictMode で例外になる。
    $targetBackends = @($Backend)
    $plans = New-Object System.Collections.Generic.List[object]
    $orphanPlans = New-Object System.Collections.Generic.List[object]
    $expectedOutputs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($job in $catalog.Jobs) {
        [void]$expectedOutputs.Add($job.OutputName)
    }

    foreach ($targetBackend in $targetBackends) {
        $outputDirectoryName = "compiled_dx12"
        $outputDirectory = Join-Path $shaderRoot $outputDirectoryName
        foreach ($job in $catalog.Jobs) {
            $outputPath = Join-Path $outputDirectory $job.OutputName
            if (Test-ShaderJobStale -Job $job -OutputPath $outputPath) {
                $plans.Add([PSCustomObject]@{
                    Backend = $targetBackend
                    OutputDirectory = $outputDirectory
                    OutputPath = $outputPath
                    Job = $job
                })
            }
        }
        if (Test-Path -LiteralPath $outputDirectory -PathType Container) {
            foreach ($compiledFile in Get-ChildItem -LiteralPath $outputDirectory -File -Filter "*.cso") {
                if (-not $expectedOutputs.Contains($compiledFile.Name)) {
                    $orphanPlans.Add([PSCustomObject]@{
                        Backend = $targetBackend
                        File = $compiledFile
                    })
                }
            }
        }
    }

    Write-Host "[compile_shaders] sources=$($catalog.SourceCount) jobs=$($catalog.Jobs.Count) stale=$($plans.Count) orphans=$($orphanPlans.Count) backend=$Backend force=$($Force.IsPresent)"

    if ($PlanOnly) {
        if (-not $SummaryOnly) {
            foreach ($plan in $plans) {
                $reason = if (-not (Test-Path -LiteralPath $plan.OutputPath -PathType Leaf)) {
                    "missing output"
                } elseif ($Force) {
                    "forced"
                } else {
                    "newer: $(Get-ShaderRelativePath -FullPath $plan.Job.Dependencies.NewestPath)"
                }
                Write-Host "[PLAN][$($plan.Backend)][$($plan.Job.Stage)] $($plan.Job.RelativePath) -> $($plan.Job.OutputName) ($reason)"
            }
            foreach ($orphanPlan in $orphanPlans) {
                Write-Host "[PLAN][$($orphanPlan.Backend)][REMOVE] orphan $($orphanPlan.File.Name)"
            }
        }
        if ($failures.Count -gt 0) {
            foreach ($failure in $failures) {
                Write-Host "[compile_shaders][ERROR][$($failure.Backend)][$($failure.Stage)] source=$($failure.Source): $($failure.Message)" -ForegroundColor Red
            }
            exit 1
        }
        exit 0
    }

    # @note 完全なno-opではログも触らず、CMakeの毎回走査を作業ツリーへの副作用なしで終える。
    if ($plans.Count -eq 0 -and $orphanPlans.Count -eq 0 -and $failures.Count -eq 0) {
        Write-Host "[compile_shaders] completed=0 skipped=$($targetBackends.Count * $catalog.Jobs.Count)"
        exit 0
    }

    Set-Content -LiteralPath $logPath -Value "[compile_shaders] $(Get-Date -Format 'o')" -Encoding UTF8
    # @note 最初のHLSLエラーで停止すると別ファイルの問題が隠れるため、ジョブ単位で失敗を集約する。
    # @note 各失敗にはバックエンド・ステージ・エントリーポイント・ソースパスを保持し、最後に全件を列挙する。
    $completedCount = 0

    foreach ($targetBackend in $targetBackends) {
        $backendPlans = @($plans | Where-Object { $_.Backend -eq $targetBackend })
        $outputDirectoryName = "compiled_dx12"
        $outputDirectory = Join-Path $shaderRoot $outputDirectoryName
        $compilerPath = $null

        if ($backendPlans.Count -gt 0) {
            try {
                $compilerPath = $selectedCompiler
            } catch {
                foreach ($plan in $backendPlans) {
                    $failures.Add([PSCustomObject]@{
                        Backend = $targetBackend
                        Stage = $plan.Job.Stage
                        Entry = $plan.Job.Entry
                        Source = $plan.Job.RelativePath
                        Message = $_.Exception.Message
                    })
                }
                Write-CompileMessage "[compile_shaders][ERROR][$targetBackend] compiler setup failed: $($_.Exception.Message)"
                continue
            }
            if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
                New-Item -ItemType Directory -Path $outputDirectory | Out-Null
            }
        }

        $backendFailureCount = $failures.Count
        foreach ($plan in $backendPlans) {
            Write-CompileMessage "[$targetBackend][$($plan.Job.Stage)][COMPILE] source=$($plan.Job.RelativePath) entry=$($plan.Job.Entry)"
            try {
                Invoke-ShaderJob -Job $plan.Job -TargetBackend $targetBackend -CompilerPath $compilerPath -OutputPath $plan.OutputPath
                $completedCount++
            } catch {
                $failure = [PSCustomObject]@{
                    Backend = $targetBackend
                    Stage = $plan.Job.Stage
                    Entry = $plan.Job.Entry
                    Source = $plan.Job.RelativePath
                    Message = $_.Exception.Message
                }
                $failures.Add($failure)
                Write-CompileMessage "[compile_shaders][ERROR][$targetBackend][$($plan.Job.Stage)] source=$($plan.Job.RelativePath) entry=$($plan.Job.Entry): $($failure.Message)"
            }
        }

        # @note 正常完了したbackendだけを掃除し、削除・改名済みソースの孤立CSOを残さない。
        if ($failures.Count -eq $backendFailureCount) {
            foreach ($orphanPlan in @($orphanPlans | Where-Object { $_.Backend -eq $targetBackend })) {
                Write-CompileMessage "[$targetBackend][REMOVE] orphan $($orphanPlan.File.Name)"
                Remove-Item -LiteralPath $orphanPlan.File.FullName -Force
                Remove-Item -LiteralPath "$($orphanPlan.File.FullName).compiler" -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($failures.Count -gt 0) {
        Write-CompileMessage "[compile_shaders][FAILED] errors=$($failures.Count) completed=$completedCount"
        for ($index = 0; $index -lt $failures.Count; $index++) {
            $failure = $failures[$index]
            Write-CompileMessage "[ERROR $($index + 1)/$($failures.Count)][$($failure.Backend)][$($failure.Stage)] source=$($failure.Source) entry=$($failure.Entry): $($failure.Message)"
        }
        exit 1
    }

    Write-CompileMessage "[compile_shaders] completed=$completedCount skipped=$($targetBackends.Count * $catalog.Jobs.Count - $plans.Count)"
    exit 0
} catch {
    $errorMessage = "[compile_shaders][FAILED] $($_.Exception.Message)"
    Write-Host $errorMessage -ForegroundColor Red
    if (-not $PlanOnly) {
        Add-Content -LiteralPath $logPath -Value $errorMessage -Encoding UTF8
    }
    exit 1
}
