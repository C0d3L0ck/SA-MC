# Compila MinHook, el plugin samc.asi y el lanzador del laboratorio (todo en 32 bits).
# Uso: powershell -ExecutionPolicy Bypass -File build.ps1 [-Instalar] [-Publico]
#   -Instalar  copia samc.asi y samc.ini a la copia de laboratorio del juego (nunca al juego real)
#   -Publico   además compila la versión para repartir (bin\publico\samc.asi): sin nada del laboratorio
param([switch]$Instalar, [switch]$Publico)

$ErrorActionPreference = 'Stop'
$codigo = $PSScriptRoot
$root   = Split-Path -Parent $codigo
# Compilador: llvm-mingw 20261006 (ucrt, x86_64) en herramientas\, o la carpeta bin que diga MOTOR_SA_LLVM
$tc     = if ($env:MOTOR_SA_LLVM) { $env:MOTOR_SA_LLVM } else { Join-Path $root 'herramientas\llvm-mingw-20261006-ucrt-x86_64\bin' }
$cc     = Join-Path $tc 'i686-w64-mingw32-clang.exe'
$cxx    = Join-Path $tc 'i686-w64-mingw32-clang++.exe'
$mh     = Join-Path $root 'terceros\minhook-1.3.4'
$obj    = Join-Path $codigo 'obj'
$out    = Join-Path $codigo 'bin'
$lab    = Join-Path $root 'lab\GTA San Andreas'
New-Item -ItemType Directory -Force -Path $obj, $out | Out-Null

function Invoke-Tool([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "Fallo $(Split-Path -Leaf $exe) (codigo $LASTEXITCODE)" }
}

# MinHook (C): solo el desensamblador de 32 bits
$mhObj = @()
foreach ($s in 'src\buffer.c', 'src\hook.c', 'src\trampoline.c', 'src\hde\hde32.c') {
    $o = Join-Path $obj ('mh_' + [IO.Path]::GetFileNameWithoutExtension($s) + '.o')
    Invoke-Tool $cc @('-O2', '-c', (Join-Path $mh $s), '-I', (Join-Path $mh 'include'), '-o', $o)
    $mhObj += $o
}

# Plugin
$src = @(Get-ChildItem -LiteralPath (Join-Path $codigo 'src') -Filter *.cpp | ForEach-Object { $_.FullName })
$asi = Join-Path $out 'samc.asi'
Invoke-Tool $cxx (@('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-microsoft-exception-spec', '-shared', '-static',
        '-I', (Join-Path $mh 'include'), '-o', $asi, "-Wl,-Map,$(Join-Path $out 'samc.map')") +
    $src + $mhObj + @('-lwinmm', '-lsynchronization'))

if ($Publico) {
    $pub = Join-Path $out 'publico'
    New-Item -ItemType Directory -Force -Path $pub | Out-Null
    Invoke-Tool $cxx (@('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-microsoft-exception-spec', '-shared', '-static',
            '-DMOTOR_SA_PUBLICO', '-I', (Join-Path $mh 'include'), '-o', (Join-Path $pub 'samc.asi'),
            "-Wl,-Map,$(Join-Path $pub 'samc.map')") + $src + $mhObj + @('-lwinmm', '-lsynchronization'))
    '{0,-16} {1,8:N0} KB  (version para repartir)' -f 'publico\samc.asi', ((Get-Item (Join-Path $pub 'samc.asi')).Length / 1KB)
    # Cargador de .asi propio (vorbisFile.dll que reenvía a la original renombrada vorbisHooked.dll)
    Invoke-Tool $cxx @('-std=c++20', '-O2', '-Wall', '-Wextra', '-shared', '-static', '-o', (Join-Path $pub 'vorbisFile.dll'),
        (Join-Path $codigo 'cargador\vorbisFile.cpp'), (Join-Path $codigo 'cargador\vorbisFile.def'))
    '{0,-16} {1,8:N0} KB  (cargador de .asi)' -f 'publico\vorbisFile.dll', ((Get-Item (Join-Path $pub 'vorbisFile.dll')).Length / 1KB)
}

# Solo se recompila un programa si alguna de sus fuentes es más nueva (el lanzador puede estar en uso por una prueba)
function Test-Stale([string]$exe, [string[]]$sources) {
    if (-not (Test-Path -LiteralPath $exe)) { return $true }
    $t = (Get-Item -LiteralPath $exe).LastWriteTime
    foreach ($s in $sources) { if ((Get-Item -LiteralPath $s).LastWriteTime -gt $t) { return $true } }
    return $false
}

# Prueba de esfuerzo del equipo de trabajadores (fuera del juego)
$tj = @((Join-Path $codigo 'pruebas\test_jobs.cpp'), (Join-Path $codigo 'src\jobs.cpp'), (Join-Path $codigo 'src\util.cpp'))
if (Test-Stale (Join-Path $out 'test_jobs.exe') $tj) {
    Invoke-Tool $cxx (@('-std=c++20', '-O2', '-static', '-o', (Join-Path $out 'test_jobs.exe')) + $tj + @('-lsynchronization'))
}

# Lanzador (32 bits, para poder inyectar samp.dll en el juego de 32 bits)
$ll = @((Join-Path $codigo 'launcher\lanzar_lab.cpp'))
if (Test-Stale (Join-Path $out 'lanzar_lab.exe') $ll) {
    Invoke-Tool $cxx (@('-std=c++20', '-O2', '-municode', '-static', '-o', (Join-Path $out 'lanzar_lab.exe')) + $ll)
}

Get-ChildItem -LiteralPath $out -File | ForEach-Object { '{0,-16} {1,8:N0} KB' -f $_.Name, ($_.Length / 1KB) }

if ($Instalar) {
    # Nombres de la version anterior (Motor SA): si quedaran, el juego cargaria el complemento dos veces
    foreach ($viejo in 'motor_sa.asi', 'motor_sa.ini') {
        $p = Join-Path $lab $viejo
        if ([IO.File]::Exists($p)) { [IO.File]::Delete($p); "Quitado del laboratorio: $viejo" }
    }
    Copy-Item -LiteralPath $asi -Destination $lab -Force
    Copy-Item -LiteralPath (Join-Path $codigo 'samc.ini') -Destination $lab -Force
    "Instalado en el laboratorio: $lab"
}
