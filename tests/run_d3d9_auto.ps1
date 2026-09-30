param([string]$Tag='auto')
$ErrorActionPreference='Stop'
if (Test-Path (Join-Path $PSScriptRoot 'd3d9.dll')) { throw 'App-local d3d9.dll must be absent.' }
$failed=0
foreach($arch in @('i686','aarch64','x86_64','arm64ec')) {
    foreach($mode in @('legacy','ex','flip')) {
        $prefix=Join-Path $PSScriptRoot "$Tag-$arch-$mode"
        $arguments=@('--system')
        if($mode -ne 'legacy') { $arguments += "--$mode" }
        $p=Start-Process (Join-Path $PSScriptRoot "probe_$arch.exe") -ArgumentList $arguments -PassThru `
            -RedirectStandardOutput "$prefix.log" -RedirectStandardError "$prefix.err"
        $null=$p.Handle
        if(!$p.WaitForExit(45000)) {
            $p.Kill(); $p.WaitForExit()
            'TIMEOUT' | Set-Content "$prefix.state"
            $failed++
        } else {
            $p.WaitForExit()
            "exit=$($p.ExitCode)" | Set-Content "$prefix.state"
            if($p.ExitCode -ne 0) { $failed++ }
        }
        $p.Dispose()
    }
}
exit ([int]($failed -ne 0))
