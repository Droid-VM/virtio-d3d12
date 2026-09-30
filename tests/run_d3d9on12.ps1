param([string]$Arch = 'aarch64', [switch]$Present, [switch]$Flip)
$ErrorActionPreference = 'Stop'
$mode = if ($Flip) { 'flip' } elseif ($Present) { 'present' } else { 'readback' }
$prefix = Join-Path $PSScriptRoot "$Arch-$mode"
Remove-Item "$prefix.state" -ErrorAction SilentlyContinue
$options = @{
    FilePath = (Join-Path $PSScriptRoot "probe_d3d9on12_$Arch.exe")
    WorkingDirectory = $PSScriptRoot
    PassThru = $true
    RedirectStandardOutput = "$prefix.log"
    RedirectStandardError = "$prefix.err"
}
if ($Present) { $options.ArgumentList = '--present' }
if ($Flip) { $options.ArgumentList = '--flip' }
$p = Start-Process @options
$null = $p.Handle
if (-not $p.WaitForExit(45000)) {
    $p.Kill()
    'TIMEOUT' | Set-Content "$prefix.state"
    exit 1
}
$p.WaitForExit()
"exit=$($p.ExitCode)" | Set-Content "$prefix.state"
exit $p.ExitCode
