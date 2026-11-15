# Portions Copyright 2026 The Hollycast Authors
$ErrorActionPreference = 'Stop'

# Load the cache policy alone: these checks require no compiler or cache daemon.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $PSScriptRoot 'build-windows.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw ($parseErrors.Message -join [Environment]::NewLine) }
$functions = $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -in @('Resolve-CacheMode', 'Invoke-NativeTextCommand')
}, $false)
foreach ($function in $functions) { . ([scriptblock]::Create($function.Extent.Text)) }

$scratch = Join-Path ([IO.Path]::GetTempPath()) ('hollycast-cache-policy-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
try {
    foreach ($mode in @('Off', 'Auto')) {
        $result = Resolve-CacheMode -RequestedMode $mode -Executable '' -MarkerPath ''
        if ($result.enabled) { throw "$mode enabled an absent compiler cache" }
    }
    $rejected = $false
    try { Resolve-CacheMode -RequestedMode 'On' -Executable '' -MarkerPath '' | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw 'Explicit On accepted an absent cache executable' }

    $executable = (Get-Process -Id $PID).Path
    $marker = Join-Path $scratch 'cache-enabled.json'
    $result = Resolve-CacheMode -RequestedMode 'Auto' -Executable $executable -MarkerPath $marker
    if ($result.enabled) { throw 'Auto enabled a cache without an enablement marker' }
    '{"enabled":true,"sha256":"wrong-executable-hash"}' | Set-Content -LiteralPath $marker
    $result = Resolve-CacheMode -RequestedMode 'Auto' -Executable $executable -MarkerPath $marker
    if ($result.enabled) { throw 'Auto accepted a marker for another executable' }
    '5 compiler-cache policy checks passed'
} finally {
    # These are the two exact paths this test created; no recursive cleanup.
    Remove-Item -LiteralPath (Join-Path $scratch 'cache-enabled.json') -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $scratch -Force
}
