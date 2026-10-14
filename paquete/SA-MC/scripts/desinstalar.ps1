# desinstalar.ps1 - quita SA:MC de la carpeta de GTA San Andreas (deja el juego como antes)
# Opciones (para pruebas): -Carpeta <ruta>  -SinPausa  -SinElevar
param([string]$Carpeta = '', [switch]$SinPausa, [switch]$SinElevar)

$ErrorActionPreference = 'Stop'
$nuestros = @('samc.asi', 'samc.ini', 'dxvk\d3d9.dll', 'samc.log', 'samc_instalado.txt',
              'motor_sa.asi', 'motor_sa.ini', 'motor_sa.log', 'motor_sa_instalado.txt')   # y los de la version anterior

function Pausa { if (-not $SinPausa) { Write-Host ''; Read-Host 'Presiona Enter para salir / Press Enter to exit' | Out-Null } }
function Existe([string]$p) { return [IO.File]::Exists($p) }

function Buscar-Juego {
    $candidatos = @()
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\SAMP' -Name 'gta_sa_exe' -ErrorAction Stop).gta_sa_exe
        if ($p) { $candidatos += [IO.Path]::GetDirectoryName($p) }
    } catch {}
    $candidatos += @(
        (Join-Path ${env:ProgramFiles(x86)} 'Rockstar Games\GTA San Andreas'),
        (Join-Path $env:ProgramFiles 'Rockstar Games\GTA San Andreas'),
        'C:\Games\GTA San Andreas', 'D:\Games\GTA San Andreas', 'C:\GTA San Andreas', 'D:\GTA San Andreas')
    foreach ($c in $candidatos) {
        if ($c -and ((Existe (Join-Path $c 'samc.asi')) -or (Existe (Join-Path $c 'motor_sa.asi')))) { return $c }
    }
    return $null
}

Write-Host '=== SA:MC: desinstalacion ===' -ForegroundColor Cyan
if (-not $Carpeta) {
    $Carpeta = Buscar-Juego
    if (-not $Carpeta) { $Carpeta = Read-Host 'No lo encontre. Escribe la carpeta de GTA San Andreas' }
}
if (-not $Carpeta -or -not (Existe (Join-Path $Carpeta 'gta_sa.exe'))) {
    Write-Host 'En esa carpeta no esta gta_sa.exe.' -ForegroundColor Red
    Pausa; exit 1
}
if (-not ((Existe (Join-Path $Carpeta 'samc.asi')) -or (Existe (Join-Path $Carpeta 'motor_sa.asi')))) {
    Write-Host "SA:MC no esta instalado en $Carpeta" -ForegroundColor Yellow
    Pausa; exit 0
}

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

foreach ($f in $nuestros) {
    $p = Join-Path $Carpeta $f
    if (Existe $p) { [IO.File]::Delete($p); Write-Host "Quitado: $f" }
}
$dx = Join-Path $Carpeta 'dxvk'
if ([IO.Directory]::Exists($dx) -and [IO.Directory]::GetFileSystemEntries($dx).Count -eq 0) { [IO.Directory]::Delete($dx) }

# Cargador de .asi de SA:MC (si lo puso el instalador): se quita solo si ningun otro .asi lo necesita
$marca = Join-Path $Carpeta 'samc_cargador.txt'
if (-not (Existe $marca)) { $marca = Join-Path $Carpeta 'motor_sa_cargador.txt' }   # instalado por la version anterior
if (Existe $marca) {
    $otros = @([IO.Directory]::GetFiles($Carpeta, '*.asi'))
    $scripts = Join-Path $Carpeta 'scripts'
    if ([IO.Directory]::Exists($scripts)) { $otros += [IO.Directory]::GetFiles($scripts, '*.asi') }
    $hooked = Join-Path $Carpeta 'vorbisHooked.dll'
    if ($otros.Count -gt 0) {
        Write-Host "Dejo el cargador de ASI (vorbisFile.dll): lo usan otros .asi ($([IO.Path]::GetFileName($otros[0]))...)." -ForegroundColor Yellow
    } elseif (Existe $hooked) {
        [IO.File]::Delete((Join-Path $Carpeta 'vorbisFile.dll'))
        [IO.File]::Move($hooked, (Join-Path $Carpeta 'vorbisFile.dll'))
        [IO.File]::Delete($marca)
        Write-Host 'Quitado el cargador de ASI: vorbisFile.dll original restaurado.'
    }
}

Write-Host ''
Write-Host 'Listo: el juego queda como antes de instalar SA:MC.' -ForegroundColor Green
Pausa
