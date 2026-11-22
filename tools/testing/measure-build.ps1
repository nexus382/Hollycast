# Portions Copyright 2026 The Hollycast Authors
[CmdletBinding()]
param(
    [string]$MeasurementDirectory = '',

    [string]$MeasurementRoot = '',

    [string]$SourceDirectory = '',

    [string]$VisualStudioPath = '',

    [string]$VisualStudioVersion = '',

    [string]$CMakePath = '',

    [string]$NinjaPath = '',

    [string]$MSVCToolsRoot = '',

    [ValidateSet('Auto', 'WindowsKits', 'NuGet')]
    [string]$WindowsSdkLayout = 'Auto',

    [string]$WindowsSdkRoot = '',

    [string]$WindowsSdkVersion = '',

    [string]$WindowsSdkLibRoot = '',

    [string]$PortableBundle = '',

    [string]$DxSdkDir = '',

    [string]$SccachePath = '',

    [string]$SccacheDirectory = '',

    [string]$SccacheMarkerPath = '',

    [bool]$EnableHarness = $true,

    [bool]$EnableHikaru = $false,

    [ValidateRange(1, 256)]
    [int]$Parallel = 16
)

$ErrorActionPreference = 'Stop'

function Get-AbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Resolve-ConfiguredValue {
    param([string]$Value, [string]$EnvironmentName, [string]$Default = '')

    if (-not [string]::IsNullOrWhiteSpace($Value)) { return $Value }
    $environmentValue = [System.Environment]::GetEnvironmentVariable($EnvironmentName, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($environmentValue)) { return $environmentValue }
    return $Default
}

function Get-SourceSignature {
    param([Parameter(Mandatory = $true)]$SourceIdentity)

    $signature = [ordered]@{
        path = [string]$SourceIdentity.path
        commit = [string]$SourceIdentity.commit
        dirty = [bool]$SourceIdentity.dirty
        status = @($SourceIdentity.status | ForEach-Object { [string]$_ })
        trackedDiffSha256 = [string]$SourceIdentity.trackedDiffSha256
        untrackedFileCount = [int]$SourceIdentity.untrackedFileCount
        untrackedFilesSha256 = [string]$SourceIdentity.untrackedFilesSha256
        submodules = @($SourceIdentity.submodules)
    }
    return $signature | ConvertTo-Json -Depth 12 -Compress
}

function Get-StableBuildOptions {
    param([Parameter(Mandatory = $true)]$BuildResult)

    $arguments = @($BuildResult.options.coreCMakeArguments)
    $stableArguments = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $arguments.Count; $index++) {
        if ([string]$arguments[$index] -eq '-B') {
            $index++
            continue
        }
        [void]$stableArguments.Add([string]$arguments[$index])
    }

    $stableOptions = [ordered]@{
        configuration = [string]$BuildResult.options.configuration
        target = [string]$BuildResult.options.target
        parallel = [int]$BuildResult.options.parallel
        coreCMakeArguments = @($stableArguments.ToArray())
        toolchain = $BuildResult.toolchain
    }
    return $stableOptions | ConvertTo-Json -Depth 6 -Compress
}

$sourcePath = if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    Get-AbsolutePath (Join-Path $PSScriptRoot '..\..')
}
else {
    Get-AbsolutePath $SourceDirectory
}

if (-not (Test-Path -LiteralPath $sourcePath -PathType Container)) {
    throw "Source directory not found: $sourcePath"
}
if (-not (Test-Path -LiteralPath (Join-Path $sourcePath 'CMakeLists.txt') -PathType Leaf)) {
    throw "Source directory does not contain CMakeLists.txt: $sourcePath"
}

$measurementRootPath = Resolve-ConfiguredValue -Value $MeasurementRoot -EnvironmentName 'HOLLYCAST_MEASUREMENT_ROOT' -Default (Join-Path ([System.IO.Path]::GetTempPath()) 'hollycast-build-measurements')
$measurementRootPath = Get-AbsolutePath $measurementRootPath
$measurementPath = if ([string]::IsNullOrWhiteSpace($MeasurementDirectory)) {
    $uniqueName = 'hollycast-measure-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    Get-AbsolutePath (Join-Path $measurementRootPath $uniqueName)
}
else {
    Get-AbsolutePath $MeasurementDirectory
}
if (Test-Path -LiteralPath $measurementPath) {
    throw "MeasurementDirectory must not already exist; refusing to reuse or delete prior data: $measurementPath"
}
$sourcePrefix = $sourcePath.TrimEnd('\') + '\'
if ($measurementPath.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase) -or
    $measurementPath.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "MeasurementDirectory must be outside the source tree: $measurementPath"
}
$configuredSccacheDirectory = Resolve-ConfiguredValue -Value $SccacheDirectory -EnvironmentName 'HOLLYCAST_SCCACHE_CACHE_DIR'
$cachePath = if ([string]::IsNullOrWhiteSpace($configuredSccacheDirectory)) { Get-AbsolutePath ($measurementPath + '.compiler-cache') } else { Get-AbsolutePath $configuredSccacheDirectory }
if ($cachePath.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase) -or
    $cachePath.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "SccacheDirectory must be outside the source tree: $cachePath"
}
$measurementPrefix = $measurementPath.TrimEnd('\') + '\'
$cachePrefix = $cachePath.TrimEnd('\') + '\'
if ($cachePath.Equals($measurementPath, [System.StringComparison]::OrdinalIgnoreCase) -or
    $cachePath.StartsWith($measurementPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
    $measurementPath.StartsWith($cachePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'MeasurementDirectory and SccacheDirectory must be separate paths.'
}
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    throw "SccacheDirectory is a file: $cachePath"
}
if (Test-Path -LiteralPath $cachePath -PathType Container) {
    $existingCacheEntries = @(Get-ChildItem -LiteralPath $cachePath -Force)
    if ($existingCacheEntries.Count -gt 0) {
        throw "SccacheDirectory must be empty for a cold-cache measurement; refusing to reuse or delete prior cache data: $cachePath"
    }
}

$buildScript = Join-Path $PSScriptRoot 'build-windows.ps1'
if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) {
    throw "Build helper not found: $buildScript"
}
$configuredSccachePath = Resolve-ConfiguredValue -Value $SccachePath -EnvironmentName 'HOLLYCAST_SCCACHE_PATH'
if ([string]::IsNullOrWhiteSpace($configuredSccachePath)) {
    $sccacheCommand = Get-Command 'sccache.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $sccacheCommand) { $configuredSccachePath = $sccacheCommand.Source }
}
if ([string]::IsNullOrWhiteSpace($configuredSccachePath) -or -not (Test-Path -LiteralPath $configuredSccachePath -PathType Leaf)) {
    throw "sccache executable not found. Set -SccachePath or HOLLYCAST_SCCACHE_PATH."
}
$configuredSccachePath = Get-AbsolutePath $configuredSccachePath

New-Item -ItemType Directory -Path $measurementPath -Force | Out-Null
if (-not (Test-Path -LiteralPath $cachePath -PathType Container)) {
    New-Item -ItemType Directory -Path $cachePath -Force | Out-Null
}

$resultPath = Join-Path $measurementPath 'measurement-result.json'
$startedUtc = [DateTime]::UtcNow
$result = [ordered]@{
    schemaVersion = 1
    startedUtc = $startedUtc.ToString('o')
    finishedUtc = $null
    sourceDirectory = $sourcePath
    measurementDirectory = $measurementPath
    compilerCacheDirectory = $cachePath
    freshBuildDirectories = $true
    cacheComparison = 'uncached full build, cold sccache full build, then warm sccache full build; each phase uses a new CMake build directory.'
    builds = @()
    sameSourceAndOptions = $null
    comparison = $null
    complete = $false
    errors = @()
}

$baselineSourceSignature = $null
$baselineOptionsSignature = $null
$phases = @(
    [ordered]@{ name = 'uncached'; cacheMode = 'Off' },
    [ordered]@{ name = 'cold-cache'; cacheMode = 'On' },
    [ordered]@{ name = 'warm-cache'; cacheMode = 'On' }
)

try {
    foreach ($phase in $phases) {
        $buildPath = Join-Path $measurementPath ("build-" + $phase.name)
        $buildResultPath = Join-Path $buildPath 'build-result.json'
        $helperError = $null
        $errorsBeforePhase = $result.errors.Count

        try {
            $helperArguments = @{
                BuildDirectory = $buildPath
                SourceDirectory = $sourcePath
                CacheMode = $phase.cacheMode
                SccachePath = $configuredSccachePath
                SccacheDirectory = $cachePath
                SccacheMarkerPath = $SccacheMarkerPath
                VisualStudioPath = $VisualStudioPath
                VisualStudioVersion = $VisualStudioVersion
                CMakePath = $CMakePath
                NinjaPath = $NinjaPath
                MSVCToolsRoot = $MSVCToolsRoot
                WindowsSdkLayout = $WindowsSdkLayout
                WindowsSdkRoot = $WindowsSdkRoot
                WindowsSdkVersion = $WindowsSdkVersion
                WindowsSdkLibRoot = $WindowsSdkLibRoot
                PortableBundle = $PortableBundle
                DxSdkDir = $DxSdkDir
                EnableHarness = $EnableHarness
                EnableHikaru = $EnableHikaru
                Parallel = $Parallel
            }
            & $buildScript @helperArguments | Out-Null
        }
        catch {
            $helperError = $_.Exception.Message
        }

        if (-not (Test-Path -LiteralPath $buildResultPath -PathType Leaf)) {
            $message = if ($helperError) { $helperError } else { 'Build helper did not write build-result.json.' }
            $result.errors += "$($phase.name): $message"
            break
        }

        try {
            $buildResult = Get-Content -LiteralPath $buildResultPath -Raw | ConvertFrom-Json -ErrorAction Stop
        }
        catch {
            $result.errors += "$($phase.name): build-result.json could not be read: $($_.Exception.Message)"
            break
        }

        $phaseSummary = [ordered]@{
            name = $phase.name
            requestedCacheMode = $phase.cacheMode
            buildDirectory = $buildPath
            resultFile = $buildResultPath
            exitCode = if ($null -ne $buildResult.exitCodes) { $buildResult.exitCodes.final } else { $null }
            configureMilliseconds = if ($null -ne $buildResult.timingsMilliseconds) { $buildResult.timingsMilliseconds.configure } else { $null }
            buildMilliseconds = if ($null -ne $buildResult.timingsMilliseconds) { $buildResult.timingsMilliseconds.build } else { $null }
            configureAndBuildMilliseconds = if ($null -ne $buildResult.timingsMilliseconds) { $buildResult.timingsMilliseconds.configureAndBuild } else { $null }
            source = $buildResult.source
            sourceAfterBuild = $buildResult.sourceAfterBuild
            options = $buildResult.options
            cacheEnabled = if ($null -ne $buildResult.cache) { [bool]$buildResult.cache.enabled } else { $false }
            cacheReason = if ($null -ne $buildResult.cache) { [string]$buildResult.cache.reason } else { $null }
            cacheStatisticsBeforeConfigure = if ($null -ne $buildResult.cache) { $buildResult.cache.statsBeforeConfigure } else { $null }
            cacheStatisticsAfterConfigure = if ($null -ne $buildResult.cache) { $buildResult.cache.statsAfterConfigure } else { $null }
            cacheStatisticsAfterBuild = if ($null -ne $buildResult.cache) { $buildResult.cache.statsAfterBuild } else { $null }
            executable = $buildResult.executable
            helperError = $helperError
        }

        $result.builds += $phaseSummary
        if ($null -eq $buildResult.source -or $null -eq $buildResult.sourceAfterBuild -or $null -eq $buildResult.options) {
            $result.errors += "$($phase.name): build helper did not produce complete source/options identity metadata."
            break
        }
        if ($null -eq $buildResult.exitCodes -or $buildResult.exitCodes.final -ne 0) {
            $failedExitCode = if ($null -ne $buildResult.exitCodes) { $buildResult.exitCodes.final } else { 'unknown' }
            $result.errors += "$($phase.name): build failed with exit code $failedExitCode."
            break
        }
        if ($null -eq $buildResult.executable -or [string]::IsNullOrWhiteSpace([string]$buildResult.executable.sha256)) {
            $result.errors += "$($phase.name): build helper did not produce an executable identity."
            break
        }

        $sourceBefore = Get-SourceSignature -SourceIdentity $buildResult.source
        $sourceAfter = Get-SourceSignature -SourceIdentity $buildResult.sourceAfterBuild
        $optionsSignature = Get-StableBuildOptions -BuildResult $buildResult

        if ($null -eq $baselineSourceSignature) {
            $baselineSourceSignature = $sourceBefore
            $baselineOptionsSignature = $optionsSignature
        }

        $sourceStayedStable = ($sourceBefore -eq $sourceAfter -and $sourceBefore -eq $baselineSourceSignature)
        $optionsStayedStable = ($optionsSignature -eq $baselineOptionsSignature)
        if (-not $sourceStayedStable) {
            $result.errors += "$($phase.name): source revision or worktree state changed during or between measured builds."
        }
        if (-not $optionsStayedStable) {
            $result.errors += "$($phase.name): fixed CMake options differ from the uncached build."
        }
        if ($phase.cacheMode -eq 'Off' -and $buildResult.cache.enabled) {
            $result.errors += 'The uncached build unexpectedly enabled sccache.'
        }
        if ($phase.cacheMode -eq 'On' -and -not $buildResult.cache.enabled) {
            $result.errors += "$($phase.name): sccache did not start, so this is not a valid cache measurement."
        }

        if ($null -ne $helperError) {
            $result.errors += "$($phase.name): build helper reported an error: $helperError"
        }
        if ($result.errors.Count -gt $errorsBeforePhase -or -not $sourceStayedStable -or -not $optionsStayedStable) {
            break
        }
    }
}
catch {
    $result.errors += "Measurement orchestration failed: $($_.Exception.Message)"
}

$result.sameSourceAndOptions = ($result.errors.Count -eq 0 -and $result.builds.Count -eq 3)
if ($result.builds.Count -eq 3) {
    $uncached = $result.builds[0]
    $cold = $result.builds[1]
    $warm = $result.builds[2]
    $warmReductionPercent = $null
    if ([double]$uncached.buildMilliseconds -gt 0 -and $null -ne $warm.buildMilliseconds) {
        $warmReductionPercent = [math]::Round((([double]$uncached.buildMilliseconds - [double]$warm.buildMilliseconds) / [double]$uncached.buildMilliseconds) * 100, 2)
    }
    $result.comparison = [ordered]@{
        uncachedBuildMilliseconds = $uncached.buildMilliseconds
        coldCacheBuildMilliseconds = $cold.buildMilliseconds
        warmCacheBuildMilliseconds = $warm.buildMilliseconds
        warmCacheBuildReductionPercent = $warmReductionPercent
        fullBuildDirectories = @($uncached.buildDirectory, $cold.buildDirectory, $warm.buildDirectory)
        warmBuildCacheStatistics = $warm.cacheStatisticsAfterBuild
    }
}

$result.complete = ($result.sameSourceAndOptions -eq $true -and $result.errors.Count -eq 0)
$result.finishedUtc = [DateTime]::UtcNow.ToString('o')
$json = $result | ConvertTo-Json -Depth 16
[System.IO.File]::WriteAllText($resultPath, $json, [System.Text.UTF8Encoding]::new($false))

if (-not $result.complete) {
    throw "Build timing measurement was incomplete. See $resultPath. $($result.errors -join ' ')"
}

Write-Output "Build timing results: $resultPath (uncached $($result.comparison.uncachedBuildMilliseconds) ms; cold cache $($result.comparison.coldCacheBuildMilliseconds) ms; warm cache $($result.comparison.warmCacheBuildMilliseconds) ms)"
