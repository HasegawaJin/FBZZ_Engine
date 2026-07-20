# FBZZ Engine
# compile_shaders.ps1 | Assets/Shaders
# HLSLの自動収集、依存差分検知、DX11/DX12向けコンパイルを一元管理する

[CmdletBinding()]
param(
    [ValidateSet("All", "DX11", "DX12")]
    [string]$Backend = "All",

    [switch]$Force,

    [switch]$PlanOnly,

    [switch]$SummaryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$shaderRoot = Split-Path -Parent $PSCommandPath
$logPath = Join-Path $shaderRoot "compile_log.txt"
$scriptTimestampUtc = (Get-Item -LiteralPath $PSCommandPath).LastWriteTimeUtc
$directIncludeCache = @{}

# WHY: Windows PowerShell 5.1にはPath.GetRelativePathがないため、ルート配下であることを
#      確認したうえで文字列として相対化し、CMake/Editorの双方から同じ命名規則を使う。
function Get-ShaderRelativePath {
    param([Parameter(Mandatory = $true)][string]$FullPath)

    $rootPrefix = $shaderRoot.TrimEnd([char[]]@(92, 47)) + [IO.Path]::DirectorySeparatorChar
    if (-not $FullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Shader path is outside the shader root: $FullPath"
    }

    return $FullPath.Substring($rootPrefix.Length).Replace([char]92, [char]47)
}

# includeは記述元の隣を優先し、その後シェーダールートを探索する。
# WHY: コンパイラーと同じ探索順にすることで、差分判定と実コンパイルの依存解決を一致させる。
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

# 各ファイルのinclude解析結果をキャッシュし、複数シェーダーが共通hlsliを使っても一度だけ読む。
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

# 推移的なincludeグラフをDFSでたどり、出力より新しい依存が一つでもあれば再コンパイルする。
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

# 全hlslを再帰収集し、ソース内に実在する標準エントリーポイントだけをジョブ化する。
# WHY: 新しいカテゴリやUIシェーダーを追加しても、手動の列挙表を更新せず自動で対象になる。
function Get-ShaderJobs {
    $jobs = New-Object System.Collections.Generic.List[object]
    $outputNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $sources = @(Get-ChildItem -LiteralPath $shaderRoot -Recurse -File -Filter "*.hlsl" | Sort-Object FullName)

    foreach ($source in $sources) {
        $relativePath = Get-ShaderRelativePath -FullPath $source.FullName
        $sourceText = [IO.File]::ReadAllText($source.FullName)
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
            throw "No VSMain, PSMain, or CSMain entry point was found: $relativePath. Include-only files must use .hlsli."
        }

        $dependencyInfo = Get-ShaderDependencyInfo -SourcePath $source.FullName
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
                throw "Multiple shaders produce the same output: $outputName"
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
    }
}

# SDK環境変数、PATH、Windows SDKの順に探索し、開発端末ごとの固定パスを不要にする。
function Find-ShaderCompiler {
    param([Parameter(Mandatory = $true)][string]$TargetBackend)

    $executableName = if ($TargetBackend -eq "DX11") { "fxc.exe" } else { "dxc.exe" }
    $overrideName = if ($TargetBackend -eq "DX11") { "FBZZ_FXC" } else { "FBZZ_DXC" }
    $overridePath = [Environment]::GetEnvironmentVariable($overrideName)
    if (-not [string]::IsNullOrWhiteSpace($overridePath)) {
        if (-not (Test-Path -LiteralPath $overridePath -PathType Leaf)) {
            throw "$overrideName points to a missing file: $overridePath"
        }
        return [IO.Path]::GetFullPath($overridePath)
    }

    $pathCommand = Get-Command $executableName -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $pathCommand) {
        return $pathCommand.Source
    }

    if (-not [string]::IsNullOrWhiteSpace($env:WindowsSdkDir) -and
        -not [string]::IsNullOrWhiteSpace($env:WindowsSDKVersion)) {
        $sdkCandidate = Join-Path $env:WindowsSdkDir "bin\$($env:WindowsSDKVersion)\x64\$executableName"
        if (Test-Path -LiteralPath $sdkCandidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($sdkCandidate)
        }
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $sdkBinRoot = Join-Path $programFilesX86 "Windows Kits\10\bin"
        if (Test-Path -LiteralPath $sdkBinRoot -PathType Container) {
            $versionDirectories = @(Get-ChildItem -LiteralPath $sdkBinRoot -Directory | Sort-Object Name -Descending)
            foreach ($versionDirectory in $versionDirectories) {
                $sdkCandidate = Join-Path $versionDirectory.FullName "x64\$executableName"
                if (Test-Path -LiteralPath $sdkCandidate -PathType Leaf) {
                    return $sdkCandidate
                }
            }
        }
    }

    throw "$executableName was not found. Install the Windows SDK or set $overrideName."
}

function Get-ShaderProfile {
    param(
        [Parameter(Mandatory = $true)][string]$TargetBackend,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $prefix = $Stage.ToLowerInvariant()
    if ($TargetBackend -eq "DX11") {
        return "${prefix}_5_0"
    }
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

    return (Get-Item -LiteralPath $OutputPath).LastWriteTimeUtc -lt $Job.Dependencies.NewestUtc
}

# 一時ファイルへのコンパイル成功後だけ本番CSOへ置換し、失敗時に前回の正常成果物を残す。
function Invoke-ShaderJob {
    param(
        [Parameter(Mandatory = $true)][object]$Job,
        [Parameter(Mandatory = $true)][string]$TargetBackend,
        [Parameter(Mandatory = $true)][string]$CompilerPath,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $temporaryPath = "$OutputPath.$([Guid]::NewGuid().ToString('N')).tmp"
    $profile = Get-ShaderProfile -TargetBackend $TargetBackend -Stage $Job.Stage

    if ($TargetBackend -eq "DX11") {
        $arguments = @(
            "/nologo", "/O3", "/I", $shaderRoot,
            "/D", "FBZZ_BACKEND_DX11=1",
            "/T", $profile, "/E", $Job.Entry,
            "/Fo", $temporaryPath, $Job.SourcePath
        )
    } else {
        $arguments = @(
            "-nologo", "-O3", "-HV", "2021", "-I", $shaderRoot,
            "-D", "FBZZ_BACKEND_DX12=1",
            "-T", $profile, "-E", $Job.Entry,
            "-Fo", $temporaryPath, $Job.SourcePath
        )
    }

    try {
        # WHY: Windows PowerShell 5.1はネイティブプロセスのstderrもErrorRecordへ変換する。
        #      コンパイラー警告でStopが発火しないよう、実行中だけContinueへ切り替える。
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
        # WHY: FXCは警告だけでも非ゼロ終了コードを返す場合があるが、正常なCSOは生成する。
        #      一意な一時CSOが生成され、かつ空でなければ警告付き成功として扱う。
        $outputWasGenerated = (Test-Path -LiteralPath $temporaryPath -PathType Leaf) -and
            ((Get-Item -LiteralPath $temporaryPath).Length -gt 0)
        if (-not $outputWasGenerated) {
            throw "Shader compilation failed with exit code $exitCode."
        }
        if ($exitCode -ne 0) {
            $warningMessage = "[compile_shaders][WARNING] Compiler returned exit code $exitCode but generated a valid shader; continuing."
            Write-Host $warningMessage -ForegroundColor Yellow
            Add-Content -LiteralPath $logPath -Value $warningMessage -Encoding UTF8
        }

        Move-Item -LiteralPath $temporaryPath -Destination $OutputPath -Force
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
    $catalog = Get-ShaderJobs
    $missingIncludes = @($catalog.Jobs | ForEach-Object { $_.Dependencies.Missing } | Sort-Object -Unique)
    if ($missingIncludes.Count -gt 0) {
        throw "Unresolved shader includes:`n  $($missingIncludes -join "`n  ")"
    }

    $targetBackends = if ($Backend -eq "All") { @("DX11", "DX12") } else { @($Backend) }
    $plans = New-Object System.Collections.Generic.List[object]
    $orphanPlans = New-Object System.Collections.Generic.List[object]
    $expectedOutputs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($job in $catalog.Jobs) {
        [void]$expectedOutputs.Add($job.OutputName)
    }

    foreach ($targetBackend in $targetBackends) {
        $outputDirectoryName = if ($targetBackend -eq "DX11") { "compiled" } else { "compiled_dx12" }
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
        exit 0
    }

    # 完全なno-opではログも触らず、CMakeの毎回走査を作業ツリーへの副作用なしで終える。
    if ($plans.Count -eq 0 -and $orphanPlans.Count -eq 0) {
        Write-Host "[compile_shaders] completed=0 skipped=$($targetBackends.Count * $catalog.Jobs.Count)"
        exit 0
    }

    Set-Content -LiteralPath $logPath -Value "[compile_shaders] $(Get-Date -Format 'o')" -Encoding UTF8

    foreach ($targetBackend in $targetBackends) {
        $backendPlans = @($plans | Where-Object { $_.Backend -eq $targetBackend })
        $outputDirectoryName = if ($targetBackend -eq "DX11") { "compiled" } else { "compiled_dx12" }
        $outputDirectory = Join-Path $shaderRoot $outputDirectoryName
        $compilerPath = $null

        if ($backendPlans.Count -gt 0) {
            $compilerPath = Find-ShaderCompiler -TargetBackend $targetBackend
            if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
                New-Item -ItemType Directory -Path $outputDirectory | Out-Null
            }
        }

        foreach ($plan in $backendPlans) {
            Write-CompileMessage "[$targetBackend][$($plan.Job.Stage)] $($plan.Job.RelativePath)"
            Invoke-ShaderJob -Job $plan.Job -TargetBackend $targetBackend -CompilerPath $compilerPath -OutputPath $plan.OutputPath
        }

        # 正常完了したbackendだけを掃除し、削除・改名済みソースの孤立CSOを残さない。
        foreach ($orphanPlan in @($orphanPlans | Where-Object { $_.Backend -eq $targetBackend })) {
            Write-CompileMessage "[$targetBackend][REMOVE] orphan $($orphanPlan.File.Name)"
            Remove-Item -LiteralPath $orphanPlan.File.FullName -Force
        }
    }

    Write-CompileMessage "[compile_shaders] completed=$($plans.Count) skipped=$($targetBackends.Count * $catalog.Jobs.Count - $plans.Count)"
    exit 0
} catch {
    $errorMessage = "[compile_shaders][FAILED] $($_.Exception.Message)"
    Write-Host $errorMessage -ForegroundColor Red
    if (-not $PlanOnly) {
        Add-Content -LiteralPath $logPath -Value $errorMessage -Encoding UTF8
    }
    exit 1
}
