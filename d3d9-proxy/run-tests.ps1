param(
    [ValidateSet('aarch64','arm64ec','x86_64')][string]$Arch = 'aarch64',
    [ValidateSet('legacy','ex','flip')][string]$Mode = 'legacy'
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $PSScriptRoot $Arch
$prefix = Join-Path $PSScriptRoot "$Arch-$Mode"
Remove-Item "$prefix.state" -ErrorAction SilentlyContinue
$options = @{
    FilePath = (Join-Path $dir 'probe_d3d9_app.exe')
    WorkingDirectory = $dir
    PassThru = $true
    RedirectStandardOutput = "$prefix.log"
    RedirectStandardError = "$prefix.err"
}
if ($Mode -eq 'ex') { $options.ArgumentList = '--ex' }
if ($Mode -eq 'flip') { $options.ArgumentList = '--flip' }
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
