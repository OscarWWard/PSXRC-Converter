param([string]$OutputPath = (Join-Path $PSScriptRoot '..\build\psxrc-gui.exe'))

$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
}
if (-not (Test-Path -LiteralPath $compiler)) {
    throw 'The .NET Framework C# compiler is required to build the Windows GUI.'
}
$destination = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$compilerArgs = @('/nologo', '/target:winexe', '/optimize+', "/out:$destination",
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll', (Join-Path $PSScriptRoot 'ConverterForm.cs'))
& $compiler @compilerArgs
exit $LASTEXITCODE
