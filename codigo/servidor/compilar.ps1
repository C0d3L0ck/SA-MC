# Compila las escenas de prueba del servidor (open.mp) y deja los .amx en servidor_omp\Server\gamemodes
# Cada bench*.pwn de esta carpeta se compila tal cual y además en su versión de base (sin autos): <nombre>_base
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$srv = Join-Path $root 'servidor_omp\Server'
$pawncc = Join-Path $srv 'qawno\pawncc.exe'
$inc = Join-Path $srv 'qawno\include'

foreach ($pwn in Get-ChildItem -LiteralPath $here -Filter 'bench*.pwn') {
    $name = [IO.Path]::GetFileNameWithoutExtension($pwn.Name)
    foreach ($variant in @(@($name, @()), @("${name}_base", @('BENCH_VEHICLES=0')))) {
        $out = Join-Path $srv "gamemodes\$($variant[0])"
        & $pawncc $pwn.FullName "-i$inc" "-i$here" "-o$out" '-;+' '-(+' '-Z-' '-d0' '-O2' @($variant[1]) | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Fallo al compilar $($variant[0]) (codigo $LASTEXITCODE)" }
        '{0,-18} {1,6} bytes' -f "$($variant[0]).amx", (Get-Item -LiteralPath "$out.amx").Length
    }
}
