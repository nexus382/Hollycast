# Portions Copyright 2026 The Hollycast Authors
[CmdletBinding()]
param(
    [string]$BuildDirectory = '',

    [string]$BuildRoot = '',

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

    [ValidateSet('Off', 'On', 'Auto')]
    [string]$CacheMode = 'Auto',

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
    param(
        [string]$Value,
        [string]$EnvironmentName,
        [string]$Default = ''
    )

    if (-not [string]::IsNullOrWhiteSpace($Value)) { return $Value }
    $environmentValue = [System.Environment]::GetEnvironmentVariable($EnvironmentName, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($environmentValue)) { return $environmentValue }
    return $Default
}

function Get-NewestVersionDirectory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $null }
    $candidates = @(Get-ChildItem -LiteralPath $Path -Directory | Where-Object { $_.Name -match '^\d+(?:\.\d+){1,3}$' } | Sort-Object { [version]$_.Name } -Descending)
    if ($candidates.Count -eq 0) { return $null }
    return $candidates[0].FullName
}

function Resolve-VisualStudioPath {
    param([string]$ConfiguredPath)

    if (-not [string]::IsNullOrWhiteSpace($ConfiguredPath)) { return (Get-AbsolutePath $ConfiguredPath) }
    if (-not [string]::IsNullOrWhiteSpace($env:VSINSTALLDIR)) { return (Get-AbsolutePath $env:VSINSTALLDIR) }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installPath = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null)
        if ($LASTEXITCODE -eq 0 -and $installPath.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace([string]$installPath[0])) {
            return (Get-AbsolutePath ([string]$installPath[0]))
        }
    }
    throw 'Visual Studio with the x86/x64 C++ tools was not found. Set -VisualStudioPath or HOLLYCAST_VS_PATH.'
}

function Resolve-ExecutablePath {
    param(
        [string]$ConfiguredPath,
        [string]$EnvironmentName,
        [string]$Name,
        [string]$VisualStudioCandidate = ''
    )

    $candidate = Resolve-ConfiguredValue -Value $ConfiguredPath -EnvironmentName $EnvironmentName
    if (-not [string]::IsNullOrWhiteSpace($candidate)) { return (Get-AbsolutePath $candidate) }
    if (-not [string]::IsNullOrWhiteSpace($VisualStudioCandidate) -and (Test-Path -LiteralPath $VisualStudioCandidate -PathType Leaf)) {
        return (Get-AbsolutePath $VisualStudioCandidate)
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) { return (Get-AbsolutePath $command.Source) }
    throw "$Name was not found. Set the matching path parameter or environment variable."
}

function Resolve-MsvcToolsRoot {
    param([string]$ConfiguredPath, [string]$VisualStudioRoot)

    $candidate = Resolve-ConfiguredValue -Value $ConfiguredPath -EnvironmentName 'HOLLYCAST_MSVC_TOOLS_ROOT'
    if ([string]::IsNullOrWhiteSpace($candidate)) { $candidate = $env:VCToolsInstallDir }
    if (-not [string]::IsNullOrWhiteSpace($candidate)) { return (Get-AbsolutePath $candidate).TrimEnd('\') }
    $newest = Get-NewestVersionDirectory -Path (Join-Path $VisualStudioRoot 'VC\Tools\MSVC')
    if ($null -eq $newest) { throw "MSVC tools were not found under '$VisualStudioRoot'. Set -MSVCToolsRoot or HOLLYCAST_MSVC_TOOLS_ROOT." }
    return $newest
}

function Resolve-WindowsSdk {
    param(
        [string]$Layout,
        [string]$BundlePath,
        [string]$RootPath,
        [string]$Version,
        [string]$LibRootPath,
        [string]$DxSdkDir
    )

    if ($Layout -eq 'Auto') { $Layout = if (-not [string]::IsNullOrWhiteSpace($BundlePath)) { 'NuGet' } else { 'WindowsKits' } }
    if ($Layout -notin @('WindowsKits', 'NuGet')) { throw "Unsupported Windows SDK layout: $Layout" }
    if ($Layout -eq 'NuGet') {
        if ([string]::IsNullOrWhiteSpace($BundlePath)) { throw 'NuGet SDK layout requires -PortableBundle or HOLLYCAST_PORTABLE_BUNDLE.' }
        $bundleRoot = Get-AbsolutePath $BundlePath
        $includeRoot = if ([string]::IsNullOrWhiteSpace($RootPath)) { Join-Path $bundleRoot 'microsoft.windows.sdk.cpp\c' } else { Get-AbsolutePath $RootPath }
        $libraryRoot = if ([string]::IsNullOrWhiteSpace($LibRootPath)) { Join-Path $bundleRoot 'microsoft.windows.sdk.cpp.x64\c' } else { Get-AbsolutePath $LibRootPath }
        $sdkVersionPath = if ([string]::IsNullOrWhiteSpace($Version)) { Get-NewestVersionDirectory -Path (Join-Path $includeRoot 'Include') } else { Join-Path (Join-Path $includeRoot 'Include') $Version }
        if ($null -eq $sdkVersionPath) { throw "No Windows SDK version was found under '$includeRoot\Include'." }
        $resolvedVersion = Split-Path -Leaf $sdkVersionPath
        $buildToolsRoot = Join-Path $bundleRoot 'microsoft.windows.sdk.buildtools\bin'
        $buildToolsVersion = Join-Path $buildToolsRoot $resolvedVersion
        if (-not (Test-Path -LiteralPath (Join-Path $buildToolsVersion 'x64') -PathType Container)) { $buildToolsVersion = Get-NewestVersionDirectory -Path $buildToolsRoot }
        if ($null -eq $buildToolsVersion) { throw "Windows SDK build tools were not found under '$buildToolsRoot'." }
        $buildToolsBin = Join-Path $buildToolsVersion 'x64'
        $dxSdk = if (-not [string]::IsNullOrWhiteSpace($DxSdkDir)) {
            Get-AbsolutePath $DxSdkDir
        }
        else {
            $candidateDxSdk = Join-Path $bundleRoot 'microsoft.dxsdk.d3dx\build\native'
            if (Test-Path -LiteralPath $candidateDxSdk -PathType Container) { $candidateDxSdk } else { '' }
        }
        return [ordered]@{ layout = $Layout; bundle = $bundleRoot; includeRoot = $includeRoot; libraryRoot = $libraryRoot; version = $resolvedVersion; includeVersionPath = $sdkVersionPath; buildToolsBin = $buildToolsBin; dxSdk = $dxSdk }
    }

    $kitsRoot = Resolve-ConfiguredValue -Value $RootPath -EnvironmentName 'HOLLYCAST_WINDOWS_SDK_ROOT'
    if ([string]::IsNullOrWhiteSpace($kitsRoot)) {
        try { $kitsRoot = (Get-ItemProperty -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -Name KitsRoot10 -ErrorAction Stop).KitsRoot10 } catch { }
    }
    if ([string]::IsNullOrWhiteSpace($kitsRoot)) { $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10' }
    $kitsRoot = Get-AbsolutePath $kitsRoot
    $sdkVersionPath = if ([string]::IsNullOrWhiteSpace($Version)) { Get-NewestVersionDirectory -Path (Join-Path $kitsRoot 'Include') } else { Join-Path (Join-Path $kitsRoot 'Include') $Version }
    if ($null -eq $sdkVersionPath) { throw "Windows Kits SDK versions were not found under '$kitsRoot\Include'." }
    $resolvedVersion = Split-Path -Leaf $sdkVersionPath
    $libraryRoot = if ([string]::IsNullOrWhiteSpace($LibRootPath)) { Join-Path $kitsRoot "Lib\$resolvedVersion" } else { Get-AbsolutePath $LibRootPath }
    $buildToolsBin = Join-Path $kitsRoot "bin\$resolvedVersion\x64"
    if (-not (Test-Path -LiteralPath $buildToolsBin -PathType Container)) { $buildToolsBin = Join-Path $kitsRoot 'bin\x64' }
    $dxSdk = if (-not [string]::IsNullOrWhiteSpace($DxSdkDir)) { Get-AbsolutePath $DxSdkDir } else { '' }
    return [ordered]@{ layout = 'WindowsKits'; bundle = $null; includeRoot = $kitsRoot; libraryRoot = $libraryRoot; version = $resolvedVersion; includeVersionPath = $sdkVersionPath; buildToolsBin = $buildToolsBin; dxSdk = $dxSdk }
}

function Get-StringSha256 {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
        return ([System.BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Invoke-GitLines {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$GitDirectory = ''
    )

    if ([string]::IsNullOrWhiteSpace($GitDirectory)) {
        $output = @(& git -C $Path --no-optional-locks @Arguments 2>&1)
    }
    else {
        $output = @(& git --no-optional-locks "--git-dir=$GitDirectory" "--work-tree=$Path" @Arguments 2>&1)
    }
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $message = ($output | ForEach-Object { [string]$_ }) -join [System.Environment]::NewLine
        throw "Git source identification failed in '$Path' (exit $exitCode): $message"
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Get-GitTreeSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [string]$GitDirectory = '',
        [switch]$IncludeSubmodules
    )

    $head = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('rev-parse', 'HEAD'))
    if ($head.Count -ne 1 -or [string]$head[0] -notmatch '^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$') {
        throw "Git source identification failed to return one HEAD commit for '$Path'."
    }

    # Hash content as well as names: a modified file can change while its Git status line stays the same.
    $status = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('status', '--short', '--untracked-files=all', '--ignore-submodules=all'))
    $trackedDiff = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('diff', '--binary', '--ignore-submodules=all', 'HEAD', '--'))
    $untrackedPaths = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('ls-files', '--others', '--exclude-standard'))
    $untrackedFiles = [System.Collections.Generic.List[string]]::new()
    foreach ($relativePath in $untrackedPaths) {
        if ([string]::IsNullOrWhiteSpace($relativePath)) { continue }
        $untrackedFile = Join-Path $Path $relativePath
        if (-not (Test-Path -LiteralPath $untrackedFile -PathType Leaf)) {
            throw "Git reported an untracked source file that cannot be hashed: $untrackedFile"
        }
        $fileHash = (Get-FileHash -LiteralPath $untrackedFile -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$untrackedFiles.Add("$relativePath`t$fileHash")
    }

    $snapshot = [ordered]@{
        path = $Path
        commit = [string]$head[0]
        dirty = ($status.Count -gt 0 -or $untrackedFiles.Count -gt 0)
        status = @($status)
        trackedDiffSha256 = Get-StringSha256 -Text ($trackedDiff -join "`n")
        untrackedFileCount = $untrackedFiles.Count
        untrackedFilesSha256 = Get-StringSha256 -Text ($untrackedFiles.ToArray() -join "`n")
        submodules = @()
    }

    if ($IncludeSubmodules) {
        $commonGitDirectoryLines = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir'))
        if ($commonGitDirectoryLines.Count -ne 1 -or [string]::IsNullOrWhiteSpace($commonGitDirectoryLines[0])) {
            throw "Git source identification failed to return a common Git directory for '$Path'."
        }
        $commonGitDirectory = Get-AbsolutePath $commonGitDirectoryLines[0]
        if (-not (Test-Path -LiteralPath $commonGitDirectory -PathType Container)) {
            throw "Git common directory does not exist for '$Path': $commonGitDirectory"
        }

        $indexLines = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('ls-files', '--stage'))
        $moduleNames = @{}
        if (@($indexLines | Where-Object { $_.StartsWith('160000 ') }).Count -gt 0) {
            $moduleFile = Join-Path $Path '.gitmodules'
            $moduleMappings = @(Invoke-GitLines -Path $Path -GitDirectory $GitDirectory -Arguments @('config', '--file', $moduleFile, '--get-regexp', '^submodule\..*\.path$'))
            foreach ($mapping in $moduleMappings) {
                $match = [regex]::Match($mapping, '^submodule\.(.+)\.path\s+(.+)$')
                if (-not $match.Success) { throw "Unable to parse submodule path mapping: $mapping" }
                $moduleNames[$match.Groups[2].Value] = $match.Groups[1].Value
            }
        }
        $gitlinkSnapshots = [System.Collections.Generic.List[object]]::new()
        foreach ($line in $indexLines) {
            $gitlinkMatch = [System.Text.RegularExpressions.Regex]::Match($line, '^160000 ([0-9a-fA-F]{40}|[0-9a-fA-F]{64}) ([0-3])\t(.+)$')
            if (-not $gitlinkMatch.Success) {
                if ($line.StartsWith('160000', [System.StringComparison]::Ordinal)) {
                    throw "Unable to parse staged Git link in '$Path': $line"
                }
                continue
            }

            $pinnedCommit = $gitlinkMatch.Groups[1].Value.ToLowerInvariant()
            $stage = [int]$gitlinkMatch.Groups[2].Value
            $relativePath = $gitlinkMatch.Groups[3].Value
            if ($stage -ne 0) {
                throw "Unmerged Git link at stage $stage cannot be fingerprinted in '$Path': $relativePath"
            }

            $submodulePath = Join-Path $Path $relativePath
            # Submodule .git files can point at stale worktree paths; resolve through this repo's common metadata.
            # Names can differ from checkout paths, as with FreeType's dlg submodule.
            $submoduleGitDirectory = $null
            if ($moduleNames.ContainsKey($relativePath)) {
                $candidateGitDirectory = Get-AbsolutePath (Join-Path (Join-Path $commonGitDirectory 'modules') $moduleNames[$relativePath])
                if (Test-Path -LiteralPath (Join-Path $candidateGitDirectory 'HEAD') -PathType Leaf) {
                    $submoduleGitDirectory = $candidateGitDirectory
                }
            }

            $worktreeExists = Test-Path -LiteralPath $submodulePath -PathType Container
            $worktreeHasFiles = $false
            if ($worktreeExists) { $worktreeHasFiles = (@(Get-ChildItem -LiteralPath $submodulePath -Force).Count -gt 0) }
            if ($null -eq $submoduleGitDirectory) {
                if ($worktreeHasFiles) { throw "Nonempty Git link worktree is missing metadata for '$relativePath' under '$commonGitDirectory'." }
                [void]$gitlinkSnapshots.Add([ordered]@{
                    path = $relativePath
                    pinnedCommit = $pinnedCommit
                    initialized = $false
                    commit = $null
                    matchesPinnedCommit = $null
                    worktree = $null
                })
                continue
            }
            if (-not $worktreeExists) {
                [void]$gitlinkSnapshots.Add([ordered]@{
                    path = $relativePath
                    pinnedCommit = $pinnedCommit
                    initialized = $false
                    commit = $null
                    matchesPinnedCommit = $null
                    worktree = $null
                })
                continue
            }

            $submoduleSnapshot = Get-GitTreeSnapshot -Path $submodulePath -GitDirectory $submoduleGitDirectory -IncludeSubmodules
            [void]$gitlinkSnapshots.Add([ordered]@{
                path = $relativePath
                pinnedCommit = $pinnedCommit
                initialized = $true
                commit = $submoduleSnapshot.commit
                matchesPinnedCommit = ($submoduleSnapshot.commit -eq $pinnedCommit)
                worktree = $submoduleSnapshot
            })
        }
        $snapshot.submodules = @($gitlinkSnapshots.ToArray())
    }

    return $snapshot
}

function Get-SourceIdentity {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-GitTreeSnapshot -Path $Path -IncludeSubmodules)
}

function Invoke-NativeTextCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $output = @(& $Executable @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $text = ($output | ForEach-Object { [string]$_ }) -join [System.Environment]::NewLine
    return [ordered]@{ exitCode = [int]$exitCode; output = $text }
}

function Invoke-TimedNativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    & $Executable @Arguments *> $LogPath
    $exitCode = $LASTEXITCODE
    $stopwatch.Stop()

    return [ordered]@{
        exitCode = [int]$exitCode
        elapsedMilliseconds = [math]::Round($stopwatch.Elapsed.TotalMilliseconds, 2)
        arguments = @($Arguments)
        logPath = $LogPath
    }
}

function Get-SccacheStatistics {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$LogPath
    )

    $command = Invoke-NativeTextCommand -Executable $Executable -Arguments @('--show-stats', '--stats-format', 'json')
    [System.IO.File]::WriteAllText($LogPath, $command.output)
    $parsed = $null
    $parseError = $null

    if ($command.exitCode -eq 0 -and -not [string]::IsNullOrWhiteSpace($command.output)) {
        try {
            $parsed = $command.output | ConvertFrom-Json -ErrorAction Stop
        }
        catch {
            $parseError = $_.Exception.Message
        }
    }

    return [ordered]@{
        exitCode = $command.exitCode
        raw = $command.output
        parsed = $parsed
        parseError = $parseError
        logPath = $LogPath
    }
}

function Get-AvailableLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return [int]$listener.LocalEndpoint.Port
    }
    finally {
        $listener.Stop()
    }
}

function Resolve-CacheMode {
    param(
        [Parameter(Mandatory = $true)][string]$RequestedMode,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Executable,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$MarkerPath
    )

    if ($RequestedMode -eq 'Off') {
        return [ordered]@{ enabled = $false; reason = 'Compiler cache was explicitly disabled.'; executableHash = $null; version = $null }
    }

    if ([string]::IsNullOrWhiteSpace($Executable) -or -not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        if ($RequestedMode -eq 'On') {
            throw "sccache executable not found: $Executable"
        }
        return [ordered]@{ enabled = $false; reason = "sccache executable not found: $Executable"; executableHash = $null; version = $null }
    }

    $resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
    $executableHash = (Get-FileHash -LiteralPath $resolvedExecutable -Algorithm SHA256).Hash.ToLowerInvariant()

    if ($RequestedMode -eq 'Auto') {
        if ([string]::IsNullOrWhiteSpace($MarkerPath) -or -not (Test-Path -LiteralPath $MarkerPath -PathType Leaf)) {
            return [ordered]@{ enabled = $false; reason = "Auto mode requires the enablement marker: $MarkerPath"; executableHash = $executableHash; version = $null }
        }

        try {
            $marker = Get-Content -LiteralPath $MarkerPath -Raw | ConvertFrom-Json -ErrorAction Stop
        }
        catch {
            return [ordered]@{ enabled = $false; reason = "Auto mode marker could not be read: $($_.Exception.Message)"; executableHash = $executableHash; version = $null }
        }

        if ($marker.enabled -ne $true -or [string]$marker.sha256 -ne $executableHash) {
            return [ordered]@{ enabled = $false; reason = 'Auto mode marker is disabled or its executable SHA256 does not match.'; executableHash = $executableHash; version = $null }
        }
    }

    $version = Invoke-NativeTextCommand -Executable $resolvedExecutable -Arguments @('--version')
    if ($version.exitCode -ne 0) {
        if ($RequestedMode -eq 'On') {
            throw "sccache --version failed with exit code $($version.exitCode): $($version.output)"
        }
        return [ordered]@{ enabled = $false; reason = "sccache --version failed with exit code $($version.exitCode)."; executableHash = $executableHash; version = $version.output }
    }

    return [ordered]@{
        enabled = $true
        reason = if ($RequestedMode -eq 'Auto') { 'Enablement marker and executable SHA256 verified.' } else { 'Compiler cache was explicitly enabled.' }
        executableHash = $executableHash
        version = $version.output
    }
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

$buildRootPath = Resolve-ConfiguredValue -Value $BuildRoot -EnvironmentName 'HOLLYCAST_BUILD_ROOT' -Default (Join-Path ([System.IO.Path]::GetTempPath()) 'hollycast-builds')
$buildRootPath = Get-AbsolutePath $buildRootPath
$buildPath = if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $uniqueName = 'hollycast-build-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    Get-AbsolutePath (Join-Path $buildRootPath $uniqueName)
}
else {
    Get-AbsolutePath $BuildDirectory
}
$sourcePrefix = $sourcePath.TrimEnd('\') + '\'
if ($buildPath.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase) -or
    $buildPath.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildDirectory must be outside the source tree: $buildPath"
}
if (Test-Path -LiteralPath $buildPath -PathType Leaf) {
    throw "BuildDirectory is a file: $buildPath"
}
if (Test-Path -LiteralPath $buildPath -PathType Container) {
    $existingEntries = @(Get-ChildItem -LiteralPath $buildPath -Force)
    if ($existingEntries.Count -gt 0) {
        throw "BuildDirectory must be empty and fresh; refusing to reuse it: $buildPath"
    }
}
else {
    New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
}

$configuredVsPath = Resolve-ConfiguredValue -Value $VisualStudioPath -EnvironmentName 'HOLLYCAST_VS_PATH'
$vsPath = Resolve-VisualStudioPath -ConfiguredPath $configuredVsPath
$cmakeCandidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninjaCandidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$cmake = Resolve-ExecutablePath -ConfiguredPath $CMakePath -EnvironmentName 'HOLLYCAST_CMAKE_PATH' -Name 'cmake.exe' -VisualStudioCandidate $cmakeCandidate
$ninja = Resolve-ExecutablePath -ConfiguredPath $NinjaPath -EnvironmentName 'HOLLYCAST_NINJA_PATH' -Name 'ninja.exe' -VisualStudioCandidate $ninjaCandidate
$msvc = Resolve-MsvcToolsRoot -ConfiguredPath $MSVCToolsRoot -VisualStudioRoot $vsPath
$compiler = Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$configuredBundle = Resolve-ConfiguredValue -Value $PortableBundle -EnvironmentName 'HOLLYCAST_PORTABLE_BUNDLE'
$configuredSdkRoot = Resolve-ConfiguredValue -Value $WindowsSdkRoot -EnvironmentName 'HOLLYCAST_WINDOWS_SDK_ROOT'
$configuredSdkLibRoot = Resolve-ConfiguredValue -Value $WindowsSdkLibRoot -EnvironmentName 'HOLLYCAST_WINDOWS_SDK_LIB_ROOT'
$configuredSdkVersion = Resolve-ConfiguredValue -Value $WindowsSdkVersion -EnvironmentName 'HOLLYCAST_WINDOWS_SDK_VERSION'
$configuredSdkLayout = if ($WindowsSdkLayout -eq 'Auto' -and -not [string]::IsNullOrWhiteSpace($env:HOLLYCAST_WINDOWS_SDK_LAYOUT)) { $env:HOLLYCAST_WINDOWS_SDK_LAYOUT } else { $WindowsSdkLayout }
$configuredDxSdk = Resolve-ConfiguredValue -Value $DxSdkDir -EnvironmentName 'HOLLYCAST_DXSDK_DIR'
$sdkInfo = Resolve-WindowsSdk -Layout $configuredSdkLayout -BundlePath $configuredBundle -RootPath $configuredSdkRoot -Version $configuredSdkVersion -LibRootPath $configuredSdkLibRoot -DxSdkDir $configuredDxSdk
$sdk = $sdkInfo.includeRoot
$sdkLib = $sdkInfo.libraryRoot
$sdkVersion = $sdkInfo.version
$buildToolsBin = $sdkInfo.buildToolsBin
$dxSdk = $sdkInfo.dxSdk
$sccacheConfiguredPath = Resolve-ConfiguredValue -Value $SccachePath -EnvironmentName 'HOLLYCAST_SCCACHE_PATH'
if ([string]::IsNullOrWhiteSpace($sccacheConfiguredPath)) {
    $sccacheCommand = Get-Command 'sccache.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $sccacheCommand) { $sccacheConfiguredPath = $sccacheCommand.Source }
}
$sccacheExecutable = if ([string]::IsNullOrWhiteSpace($sccacheConfiguredPath)) { '' } else { Get-AbsolutePath $sccacheConfiguredPath }
$defaultCacheDirectory = if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { Join-Path ([System.IO.Path]::GetTempPath()) 'hollycast-compiler-cache' } else { Join-Path $env:LOCALAPPDATA 'Hollycast\compiler-cache' }
$sccacheConfiguredDirectory = Resolve-ConfiguredValue -Value $SccacheDirectory -EnvironmentName 'HOLLYCAST_SCCACHE_CACHE_DIR' -Default $defaultCacheDirectory
$sccacheCache = Get-AbsolutePath $sccacheConfiguredDirectory
$vsVersion = Resolve-ConfiguredValue -Value $VisualStudioVersion -EnvironmentName 'HOLLYCAST_VS_VERSION' -Default '17.0'
$markerPath = if ([string]::IsNullOrWhiteSpace($SccacheMarkerPath)) {
    $configuredMarker = [System.Environment]::GetEnvironmentVariable('HOLLYCAST_SCCACHE_MARKER', 'Process')
    if (-not [string]::IsNullOrWhiteSpace($configuredMarker)) { Get-AbsolutePath $configuredMarker }
    elseif (-not [string]::IsNullOrWhiteSpace($sccacheExecutable)) { Join-Path (Split-Path -Parent $sccacheExecutable) 'cache-enabled.json' }
    else { '' }
}
else {
    Get-AbsolutePath $SccacheMarkerPath
}

$resultPath = Join-Path $buildPath 'build-result.json'
$startedUtc = [DateTime]::UtcNow
$originalLocation = Get-Location
$environmentNames = @(
    'PATH', 'INCLUDE', 'LIB', 'WindowsSdkDir', 'WindowsSDKVersion', 'VisualStudioVersion',
    'VSCMD_ARG_TGT_ARCH', 'DXSDK_DIR', 'SCCACHE_CONF', 'SCCACHE_DIR', 'SCCACHE_CACHE_SIZE',
    'SCCACHE_SERVER_PORT', 'SCCACHE_MULTILEVEL_CHAIN', 'CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER'
)
$originalEnvironment = @{}
foreach ($name in $environmentNames) {
    $originalEnvironment[$name] = [System.Environment]::GetEnvironmentVariable($name, 'Process')
}

$result = [ordered]@{
    schemaVersion = 1
    startedUtc = $startedUtc.ToString('o')
    source = $null
    sourceAfterBuild = $null
    toolchain = $null
    options = $null
    cache = [ordered]@{
        requestedMode = $CacheMode
        enabled = $false
        reason = $null
        executable = $sccacheExecutable
        executableSha256 = $null
        version = $null
        directory = $sccacheCache
        sizeLimit = '5G'
        serverPort = $null
        marker = $markerPath
        statsBeforeConfigure = $null
        statsAfterConfigure = $null
        statsAfterBuild = $null
        serverStartExitCode = $null
        serverStopExitCode = $null
    }
    timingsMilliseconds = [ordered]@{ configure = $null; build = $null; configureAndBuild = $null }
    exitCodes = [ordered]@{ configure = $null; build = $null; final = 1 }
    logs = [ordered]@{ configure = (Join-Path $buildPath 'configure.log'); build = (Join-Path $buildPath 'build.log') }
    executable = $null
    errors = @()
}

$serverStarted = $false
$configureResult = $null
$buildResult = $null
$resolvedCacheMode = $null

try {
    $requiredPaths = @(
        $cmake,
        $ninja,
        $compiler,
        $buildToolsBin,
        (Join-Path $sdk "Include\$sdkVersion\ucrt"),
        (Join-Path $sdk "Include\$sdkVersion\shared"),
        (Join-Path $sdk "Include\$sdkVersion\um"),
        (Join-Path $sdk "Include\$sdkVersion\winrt"),
        (Join-Path $sdk "Include\$sdkVersion\cppwinrt"),
        (Join-Path $sdkLib 'ucrt\x64'),
        (Join-Path $sdkLib 'um\x64')
    )
    foreach ($requiredPath in $requiredPaths) {
        if (-not (Test-Path -LiteralPath $requiredPath)) {
            throw "Required Windows build tool or SDK path not found: $requiredPath"
        }
    }

    $harnessCMakeLists = Join-Path $sourcePath 'core\ui\CMakeLists.txt'
    $harnessOptionExists = (Test-Path -LiteralPath $harnessCMakeLists -PathType Leaf) -and
        [bool](Select-String -LiteralPath $harnessCMakeLists -Pattern '^\s*option\s*\(\s*HOLLYCAST_TEST_HARNESS\b' -Quiet)
    if ($EnableHarness -and -not $harnessOptionExists) {
        throw 'Native test harness was requested, but this source revision does not define HOLLYCAST_TEST_HARNESS.'
    }
    if ($EnableHikaru) {
        if (-not (Test-Path -LiteralPath (Join-Path $sourcePath 'core\hw\hikaru') -PathType Container)) {
            throw 'Hikaru was requested, but core/hw/hikaru is absent from this source revision.'
        }
        if (-not (Select-String -LiteralPath (Join-Path $sourcePath 'CMakeLists.txt') -Pattern '\bUSE_HIKARU\b' -Quiet)) {
            throw 'Hikaru was requested, but the root CMake configuration does not expose USE_HIKARU.'
        }
    }

    $buildPrefix = $buildPath.TrimEnd('\') + '\'
    $cachePrefix = $sccacheCache.TrimEnd('\') + '\'
    if ($sccacheCache.Equals($sourcePath, [System.StringComparison]::OrdinalIgnoreCase) -or $sccacheCache.StartsWith($sourcePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "SccacheDirectory must be outside the source tree: $sccacheCache"
    }
    if ($sccacheCache.Equals($buildPath, [System.StringComparison]::OrdinalIgnoreCase) -or
        $sccacheCache.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or
        $buildPath.StartsWith($cachePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw 'BuildDirectory and SccacheDirectory must be separate paths.'
    }

    $resolvedCacheMode = Resolve-CacheMode -RequestedMode $CacheMode -Executable $sccacheExecutable -MarkerPath $markerPath
    $result.cache.enabled = [bool]$resolvedCacheMode.enabled
    $result.cache.reason = $resolvedCacheMode.reason
    $result.cache.executableSha256 = $resolvedCacheMode.executableHash
    $result.cache.version = $resolvedCacheMode.version

    $cmakeVersion = Invoke-NativeTextCommand -Executable $cmake -Arguments @('--version')
    $ninjaVersion = Invoke-NativeTextCommand -Executable $ninja -Arguments @('--version')
    if ($cmakeVersion.exitCode -ne 0) { throw "cmake --version failed: $($cmakeVersion.output)" }
    if ($ninjaVersion.exitCode -ne 0) { throw "ninja --version failed: $($ninjaVersion.output)" }

    $result.source = Get-SourceIdentity -Path $sourcePath
    $result.toolchain = [ordered]@{
        visualStudio = $vsPath
        visualStudioVersion = $vsVersion
        cmake = $cmake
        cmakeVersion = $cmakeVersion.output.Trim()
        ninja = $ninja
        ninjaVersion = $ninjaVersion.output.Trim()
        compiler = $compiler
        compilerFileVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion
        msvcToolsVersion = Split-Path -Leaf $msvc
        msvcToolsRoot = $msvc
        windowsSdkLayout = $sdkInfo.layout
        windowsSdkRoot = $sdk
        windowsSdkLibraryRoot = $sdkLib
        sdkVersion = $sdkVersion
        dxSdk = if ([string]::IsNullOrWhiteSpace($dxSdk)) { $null } else { $dxSdk }
        generator = 'Ninja'
    }

    $originalPath = [System.Environment]::GetEnvironmentVariable('PATH', 'Process')
    $env:PATH = "$msvc\bin\Hostx64\x64;$buildToolsBin;$(Split-Path -Parent $cmake);$(Split-Path -Parent $ninja);$originalPath"
    $env:INCLUDE = "$msvc\include;$msvc\atlmfc\include;$(Join-Path $sdk "Include\$sdkVersion\ucrt");$(Join-Path $sdk "Include\$sdkVersion\shared");$(Join-Path $sdk "Include\$sdkVersion\um");$(Join-Path $sdk "Include\$sdkVersion\winrt");$(Join-Path $sdk "Include\$sdkVersion\cppwinrt")"
    $env:LIB = "$msvc\lib\x64;$msvc\atlmfc\lib\x64;$(Join-Path $sdkLib 'ucrt\x64');$(Join-Path $sdkLib 'um\x64')"
    $env:WindowsSdkDir = "$sdk\"
    $env:WindowsSDKVersion = "$sdkVersion\"
    $env:VisualStudioVersion = $vsVersion
    $env:VSCMD_ARG_TGT_ARCH = 'x64'
    $env:DXSDK_DIR = $dxSdk
    $env:CMAKE_C_COMPILER_LAUNCHER = ''
    $env:CMAKE_CXX_COMPILER_LAUNCHER = ''
    Remove-Item Env:\SCCACHE_CONF -ErrorAction SilentlyContinue
    Remove-Item Env:\SCCACHE_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:\SCCACHE_CACHE_SIZE -ErrorAction SilentlyContinue
    Remove-Item Env:\SCCACHE_SERVER_PORT -ErrorAction SilentlyContinue

    $launcher = ''
    if ($resolvedCacheMode.enabled) {
        if (Test-Path -LiteralPath $sccacheCache -PathType Leaf) {
            throw "SccacheDirectory is a file: $sccacheCache"
        }
        if (-not (Test-Path -LiteralPath $sccacheCache -PathType Container)) {
            New-Item -ItemType Directory -Path $sccacheCache -Force | Out-Null
        }

        # Isolate the server and config from other sccache clients and user-level settings.
        $configPath = Join-Path $buildPath 'sccache-local.toml'
        [System.IO.File]::WriteAllText($configPath, "`n", [System.Text.UTF8Encoding]::new($false))
        $env:SCCACHE_CONF = $configPath
        $env:SCCACHE_DIR = $sccacheCache
        $env:SCCACHE_CACHE_SIZE = '5G'
        $env:SCCACHE_MULTILEVEL_CHAIN = 'disk'
        $env:SCCACHE_SERVER_PORT = [string](Get-AvailableLoopbackPort)
        $result.cache.serverPort = $env:SCCACHE_SERVER_PORT

        $launcher = $sccacheExecutable
        $serverStart = Invoke-NativeTextCommand -Executable $sccacheExecutable -Arguments @('--start-server')
        $result.cache.serverStartExitCode = $serverStart.exitCode
        [System.IO.File]::WriteAllText((Join-Path $buildPath 'sccache-server-start.log'), $serverStart.output)
        if ($serverStart.exitCode -ne 0) {
            throw "Isolated sccache server failed to start: $($serverStart.output)"
        }
        $serverStarted = $true
        $result.cache.statsBeforeConfigure = Get-SccacheStatistics -Executable $sccacheExecutable -LogPath (Join-Path $buildPath 'sccache-stats-before-configure.json')
    }

    $debugInfoArgument = '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded'
    $policyArgument = '-DCMAKE_POLICY_DEFAULT_CMP0141=NEW'
    $harnessValue = if ($EnableHarness) { 'ON' } else { 'OFF' }
    $coreConfigureArguments = @(
        '-S', $sourcePath,
        '-B', $buildPath,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_C_COMPILER=cl',
        '-DCMAKE_CXX_COMPILER=cl',
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DHOLLYCAST_TEST_HARNESS=$harnessValue",
        '-DBUILD_TESTING=OFF',
        '-DENABLE_CTEST=OFF',
        '-DTEST_AUTOMATION=OFF',
        '-DUSE_DX11=ON',
        '-DUSE_DX9=OFF',
        '-DUSE_OPENGL=ON',
        '-DUSE_VULKAN=ON',
        '-DUSE_OPENMP=ON',
        '-DUSE_DISCORD=OFF',
        '-DUSE_BREAKPAD=OFF',
        '-DUSE_LUA=OFF',
        $debugInfoArgument,
        $policyArgument
    )
    if (-not $harnessOptionExists) {
        $coreConfigureArguments = @($coreConfigureArguments | Where-Object { -not $_.StartsWith('-DHOLLYCAST_TEST_HARNESS=') })
    }
    if ($EnableHikaru) { $coreConfigureArguments += '-DUSE_HIKARU=ON' }
    $launcherArguments = @(
        "-DCMAKE_C_COMPILER_LAUNCHER=$launcher",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher"
    )
    $configureArguments = @($coreConfigureArguments) + @($launcherArguments)
    $result.options = [ordered]@{
        configuration = 'Release'
        target = 'Hollycast'
        parallel = $Parallel
        enableHarness = $EnableHarness
        enableHikaru = $EnableHikaru
        coreCMakeArguments = @($coreConfigureArguments)
        compilerLauncherArguments = @($launcherArguments)
        buildArguments = @('--build', $buildPath, '--target', 'Hollycast', '--parallel', [string]$Parallel)
        cacheMode = if ($resolvedCacheMode.enabled) { 'On' } else { 'Off' }
    }

    Push-Location -LiteralPath $sourcePath
    try {
        $configureResult = Invoke-TimedNativeCommand -Executable $cmake -Arguments $configureArguments -LogPath $result.logs.configure
        $result.exitCodes.configure = $configureResult.exitCode
        $result.timingsMilliseconds.configure = $configureResult.elapsedMilliseconds

        if ($result.cache.enabled) {
            $result.cache.statsAfterConfigure = Get-SccacheStatistics -Executable $sccacheExecutable -LogPath (Join-Path $buildPath 'sccache-stats-after-configure.json')
        }

        if ($configureResult.exitCode -eq 0) {
            $buildArguments = @('--build', $buildPath, '--target', 'Hollycast', '--parallel', [string]$Parallel)
            $buildResult = Invoke-TimedNativeCommand -Executable $cmake -Arguments $buildArguments -LogPath $result.logs.build
            $result.exitCodes.build = $buildResult.exitCode
            $result.timingsMilliseconds.build = $buildResult.elapsedMilliseconds
            $result.timingsMilliseconds.configureAndBuild = [math]::Round($configureResult.elapsedMilliseconds + $buildResult.elapsedMilliseconds, 2)
        }
        else {
            $result.errors += "CMake configure failed with exit code $($configureResult.exitCode)."
        }
    }
    finally {
        Pop-Location
    }

    if ($serverStarted -and $null -ne $buildResult) {
        $result.cache.statsAfterBuild = Get-SccacheStatistics -Executable $sccacheExecutable -LogPath (Join-Path $buildPath 'sccache-stats-after-build.json')
    }

    $result.sourceAfterBuild = Get-SourceIdentity -Path $sourcePath
    $result.exitCodes.final = if ($null -ne $buildResult) { $buildResult.exitCode } elseif ($null -ne $configureResult) { $configureResult.exitCode } else { 1 }
    $sourceBeforeBuildJson = $result.source | ConvertTo-Json -Depth 20 -Compress
    $sourceAfterBuildJson = $result.sourceAfterBuild | ConvertTo-Json -Depth 20 -Compress
    if ($sourceBeforeBuildJson -ne $sourceAfterBuildJson) {
        $result.errors += 'Source revision or worktree state changed during the build.'
        if ($result.exitCodes.final -eq 0) { $result.exitCodes.final = 1 }
    }
}
catch {
    $result.errors += $_.Exception.Message
    if ($result.exitCodes.final -eq 0) {
        $result.exitCodes.final = 1
    }
}
finally {
    if ($serverStarted) {
        try {
            $serverStop = Invoke-NativeTextCommand -Executable $sccacheExecutable -Arguments @('--stop-server')
            $result.cache.serverStopExitCode = $serverStop.exitCode
            [System.IO.File]::WriteAllText((Join-Path $buildPath 'sccache-server-stop.log'), $serverStop.output)
            if ($serverStop.exitCode -ne 0) {
                $result.errors += "Isolated sccache server cleanup failed with exit code $($serverStop.exitCode)."
                if ($result.exitCodes.final -eq 0) { $result.exitCodes.final = 1 }
            }
        }
        catch {
            $result.errors += "Isolated sccache server cleanup failed: $($_.Exception.Message)"
            if ($result.exitCodes.final -eq 0) { $result.exitCodes.final = 1 }
        }
    }

    foreach ($name in $environmentNames) {
        $value = $originalEnvironment[$name]
        [System.Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
    Set-Location -LiteralPath $originalLocation.Path

    $result.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $result.executable = $null
    try {
        $executablePath = Join-Path $buildPath 'Hollycast.exe'
        if (Test-Path -LiteralPath $executablePath -PathType Leaf) {
            $executableFile = Get-Item -LiteralPath $executablePath
            $result.executable = [ordered]@{
                path = $executableFile.FullName
                length = $executableFile.Length
                sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash.ToLowerInvariant()
                lastWriteTimeUtc = $executableFile.LastWriteTimeUtc.ToString('o')
            }
        }
    }
    catch {
        $result.errors += "Executable identity could not be recorded: $($_.Exception.Message)"
        if ($result.exitCodes.final -eq 0) { $result.exitCodes.final = 1 }
    }
    if ($result.exitCodes.final -eq 0 -and $null -eq $result.executable) {
        $result.errors += 'The Hollycast build target completed without producing Hollycast.exe.'
        $result.exitCodes.final = 1
    }

    $json = $result | ConvertTo-Json -Depth 14
    [System.IO.File]::WriteAllText($resultPath, $json, [System.Text.UTF8Encoding]::new($false))
}

if ($result.exitCodes.final -ne 0) {
    throw "Windows build failed with exit code $($result.exitCodes.final). See $resultPath."
}

Write-Output "Build result: $resultPath (configure $($result.timingsMilliseconds.configure) ms, build $($result.timingsMilliseconds.build) ms)"
