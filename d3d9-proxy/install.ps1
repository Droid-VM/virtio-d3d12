param([Parameter(Mandatory=$true)][string]$Application, [switch]$Remove)
$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $Application).Path
if ([IO.Path]::GetExtension($app) -ine '.exe') { throw 'Application must be an executable file.' }
$stream = [IO.File]::OpenRead($app)
$reader = New-Object IO.BinaryReader($stream)
try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { throw 'Invalid DOS header.' }
    $stream.Position = 0x3C
    $pe = $reader.ReadUInt32()
    if ($pe -lt 64 -or $pe -gt $stream.Length - 6) { throw 'Invalid PE offset.' }
    $stream.Position = $pe
    if ($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE signature.' }
    $machine = $reader.ReadUInt16()
} finally { $reader.Dispose() }
$arch = switch ($machine) {
    0xAA64 { 'aarch64' }
    0x8664 { 'x86_64' }
    0xA641 { 'x86_64' }
    0x014C { throw '32-bit x86 applications are not supported by this package.' }
    default { throw ('Unsupported PE machine: 0x{0:X4}' -f $machine) }
}
$source = Join-Path (Join-Path $PSScriptRoot $arch) 'd3d9.dll'
$target = Join-Path ([IO.Path]::GetDirectoryName($app)) 'd3d9.dll'
if ($target.StartsWith([IO.Path]::GetFullPath($env:windir).TrimEnd('\') + '\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'This installer only supports application directories outside Windows.'
}
$hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
if (Test-Path -LiteralPath $target) {
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) {
        throw 'An existing different d3d9.dll is present. It has not been changed.'
    }
    if ($Remove) { Remove-Item -LiteralPath $target; "Removed $target" }
    else { "Already installed: $target" }
} elseif ($Remove) {
    "Not installed: $target"
} else {
    [IO.File]::Copy($source,$target,$false)
    "Installed $arch proxy: $target"
}
