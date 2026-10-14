# instalar.ps1 - instala SA:MC en la carpeta de GTA San Andreas (1.0 US)
# Lo abre instalar.bat. Copia: samc.asi, samc.ini y dxvk\d3d9.dll. No reemplaza ningun archivo del juego: si ya
# habia una version anterior (tambien la de nombre Motor SA: motor_sa.asi/.ini), la guarda en "samc_respaldo_<fecha>"
# y quita los archivos viejos (si quedaran, el juego cargaria el complemento dos veces).
# Opciones (para pruebas): -Carpeta <ruta>  -SinPausa  -SinElevar
param([string]$Carpeta = '', [switch]$SinPausa, [switch]$SinElevar)

$ErrorActionPreference = 'Stop'
$paquete = Split-Path -Parent $PSScriptRoot
$origen = Join-Path $paquete 'archivos'
$nuestros = @('samc.asi', 'samc.ini', 'dxvk\d3d9.dll')
$viejos = @('motor_sa.asi', 'motor_sa.ini', 'motor_sa_instalado.txt')   # version anterior (Motor SA)

function Pausa { if (-not $SinPausa) { Write-Host ''; Read-Host 'Presiona Enter para salir / Press Enter to exit' | Out-Null } }
function Existe([string]$p) { return [IO.File]::Exists($p) }

function Buscar-Juego {
    $candidatos = @()
    # SA-MP guarda la ruta de gta_sa.exe en el registro
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\SAMP' -Name 'gta_sa_exe' -ErrorAction Stop).gta_sa_exe
        if ($p) { $candidatos += [IO.Path]::GetDirectoryName($p) }
    } catch {}
    $candidatos += @(
        (Join-Path ${env:ProgramFiles(x86)} 'Rockstar Games\GTA San Andreas'),
        (Join-Path $env:ProgramFiles 'Rockstar Games\GTA San Andreas'),
        'C:\Games\GTA San Andreas', 'D:\Games\GTA San Andreas', 'C:\GTA San Andreas', 'D:\GTA San Andreas')
    foreach ($c in $candidatos) {
        if ($c -and (Existe (Join-Path $c 'gta_sa.exe'))) { return $c }
    }
    return $null
}

function Elegir-Carpeta {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $d = New-Object System.Windows.Forms.FolderBrowserDialog
        $d.Description = 'Elige la carpeta de GTA San Andreas (donde esta gta_sa.exe)'
        if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    } catch {}
    return (Read-Host 'Escribe la carpeta de GTA San Andreas')
}

Write-Host '=== SA:MC: instalacion ===' -ForegroundColor Cyan
if (-not (Existe (Join-Path $origen 'samc.asi'))) {
    Write-Host "No encuentro los archivos del paquete en: $origen" -ForegroundColor Red
    Pausa; exit 1
}

# 1) Carpeta del juego
if (-not $Carpeta) {
    $Carpeta = Buscar-Juego
    if ($Carpeta) {
        Write-Host "Encontre GTA San Andreas en: $Carpeta"
        $r = Read-Host 'Instalar ahi? (S/N)'
        if ($r -notmatch '^[sSyY]') { $Carpeta = Elegir-Carpeta }
    } else {
        Write-Host 'No encontre GTA San Andreas automaticamente.'
        $Carpeta = Elegir-Carpeta
    }
}
if (-not $Carpeta -or -not (Existe (Join-Path $Carpeta 'gta_sa.exe'))) {
    Write-Host 'En esa carpeta no esta gta_sa.exe. No instalo nada.' -ForegroundColor Red
    Pausa; exit 1
}

# 2) Permisos: en "Archivos de programa" hace falta ser administrador
$prueba = Join-Path $Carpeta 'samc_prueba_escritura.tmp'
try {
    [IO.File]::WriteAllText($prueba, 'ok')
    [IO.File]::Delete($prueba)
} catch {
    Write-Host ('No puedo escribir en la carpeta del juego: ' + $_.Exception.Message) -ForegroundColor Yellow
    if ($SinElevar) { Pausa; exit 1 }
    Write-Host 'Hacen falta permisos de administrador: Windows te los va a pedir.' -ForegroundColor Yellow
    $argv = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Carpeta `"$Carpeta`""
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argv -Verb RunAs
    exit 0
}

# 3) Version del juego
Write-Host 'SA:MC comprueba la version del juego al iniciar: si no es la 1.0 US, no hace nada' -ForegroundColor Cyan
Write-Host '(el juego queda como siempre).' -ForegroundColor Cyan

# 4) Cargador de ASI (sin el, el juego no abre samc.asi)
$cargador = $null
if (Existe (Join-Path $Carpeta 'vorbisHooked.dll')) {
    $cargador = if ((Existe (Join-Path $Carpeta 'samc_cargador.txt')) -or (Existe (Join-Path $Carpeta 'motor_sa_cargador.txt'))) { 'el de SA:MC (vorbisFile.dll)' } else { "Silent's ASI Loader (vorbisFile.dll)" }
}
foreach ($n in 'dinput8.dll', 'version.dll', 'd3d8.dll', 'wininet.dll', 'xlive.dll') {
    if (-not $cargador -and (Existe (Join-Path $Carpeta $n))) { $cargador = "posiblemente Ultimate ASI Loader ($n)" }
}
$otros = @([IO.Directory]::GetFiles($Carpeta, '*.asi') | Where-Object { @('samc.asi', 'motor_sa.asi') -notcontains [IO.Path]::GetFileName($_) })
if (-not $cargador -and $otros.Count -gt 0) { $cargador = "ya usas otros .asi ($([IO.Path]::GetFileName($otros[0]))...)" }
$instalarCargador = $false
$vorbis = Join-Path $Carpeta 'vorbisFile.dll'
if ($cargador) {
    Write-Host "Cargador de ASI: $cargador" -ForegroundColor Green
} elseif ((Existe $vorbis) -and ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($vorbis)).Contains('ov_read_float'))) {
    # vorbisFile.dll es la biblioteca Ogg Vorbis original del juego (los cargadores no traen ov_read_float)
    Write-Host 'No encontre un cargador de ASI (sin el, el juego no abre samc.asi).' -ForegroundColor Yellow
    Write-Host 'SA:MC trae uno propio: vorbisFile.dll (el original queda como vorbisHooked.dll; desinstalar.bat lo devuelve).'
    $r = if ($SinPausa) { 's' } else { Read-Host 'Instalar el cargador de SA:MC? (S/N)' }
    $instalarCargador = $r -match '^[sSyY]'
} else {
    Write-Host 'ATENCION: no encontre un cargador de ASI. Sin el, el juego no carga samc.asi.' -ForegroundColor Yellow
    Write-Host "Instala uno (por ejemplo Silent's ASI Loader o Ultimate ASI Loader) y vuelve a abrir el juego." -ForegroundColor Yellow
}
if (Existe (Join-Path $Carpeta 'd3d9.dll')) {
    Write-Host 'Nota: hay un d3d9.dll en la carpeta del juego (ENB, ReShade u otro). Con SA:MC el dibujo va por' -ForegroundColor Yellow
    Write-Host 'dxvk\d3d9.dll; para seguir usando el tuyo, pon d3d9_dll=d3d9.dll en samc.ini (sin DXVK).' -ForegroundColor Yellow
}

# 5) Respaldo de una version anterior de SA:MC y copia
$previos = @(($nuestros + $viejos) | Where-Object { Existe (Join-Path $Carpeta $_) })
if ($previos.Count -gt 0) {
    $resp = Join-Path $Carpeta ('samc_respaldo_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
    foreach ($f in $previos) {
        $dst = Join-Path $resp $f
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dst))
        [IO.File]::Copy((Join-Path $Carpeta $f), $dst, $true)
    }
    Write-Host "Version anterior de SA:MC guardada en: $resp"
}
foreach ($f in $viejos) {
    $p = Join-Path $Carpeta $f
    if (Existe $p) { [IO.File]::Delete($p); Write-Host "Quitado (version anterior): $f" }
}
$marcaVieja = Join-Path $Carpeta 'motor_sa_cargador.txt'
if (Existe $marcaVieja) { [IO.File]::Move($marcaVieja, (Join-Path $Carpeta 'samc_cargador.txt')) }
[void][IO.Directory]::CreateDirectory((Join-Path $Carpeta 'dxvk'))
foreach ($f in $nuestros) { [IO.File]::Copy((Join-Path $origen $f), (Join-Path $Carpeta $f), $true) }
[IO.File]::WriteAllLines((Join-Path $Carpeta 'samc_instalado.txt'),
    [string[]](@("SA:MC instalado el $(Get-Date -Format 'yyyy-MM-dd HH:mm'). Archivos:") + $nuestros))
if ($instalarCargador) {
    [IO.File]::Move($vorbis, (Join-Path $Carpeta 'vorbisHooked.dll'))
    [IO.File]::Copy((Join-Path $origen 'cargador\vorbisFile.dll'), $vorbis, $false)
    [IO.File]::WriteAllText((Join-Path $Carpeta 'samc_cargador.txt'),
        'vorbisFile.dll es el cargador de .asi de SA:MC; el vorbisFile.dll original del juego es vorbisHooked.dll')
    Write-Host 'Cargador de ASI de SA:MC instalado (vorbisFile.dll).' -ForegroundColor Green
}

Write-Host ''
Write-Host 'Listo. Abre el juego como siempre (un jugador o SA-MP).' -ForegroundColor Green
Write-Host 'Las opciones estan en samc.ini (Bloc de notas). Para quitarlo: desinstalar.bat' -ForegroundColor Green
Pausa
