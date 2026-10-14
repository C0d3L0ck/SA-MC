# Ejecuta una prueba de rendimiento en la copia de laboratorio del juego y muestra el resumen.
# Uso:
#   prueba.ps1 -Etiqueta humo_sp -Modo sp                          (un jugador, partida nueva)
#   prueba.ps1 -Etiqueta samp_100 -Modo samp -Escena bench         (SA-MP, 100 autos en el aeropuerto)
#   prueba.ps1 -Etiqueta samp_base -Modo samp -Escena bench_base   (SA-MP, misma escena sin autos)
#   prueba.ps1 -Etiqueta samp_1000 -Modo samp -Escena bench_1000 -Radio 400   (SA-MP, 1000 autos en la pista)
# El juego se abre en pantalla, espera a que cargue, mide y se cierra solo.
param(
    [Parameter(Mandatory = $true)][string]$Etiqueta,
    [ValidateSet('sp', 'samp')][string]$Modo = 'sp',
    [string]$Escena = 'bench',
    [ValidateRange(50, 400)][int]$Radio = 200,
    [int]$Espera = 25,
    [int]$Medicion = 30,
    [int]$Limite = 180,
    [switch]$Muestreo,
    [switch]$MuestreoHilo,
    [switch]$D3D,
    [switch]$Hilo,
    [switch]$Dxvk,
    [int]$RuedasM = 0,
    [switch]$Elegida,   # la configuracion elegida: fila de dibujo + DXVK + ruedas lejanas a 150 m
    [int]$Foco = 0,     # SOLO LABORATORIO: cada cuantos segundos de medicion minimizar y restaurar el juego
    [int]$Lotes = 0,    # juntar dibujos de autos: 0 = no, 1 = ordenados por pieza
    [switch]$DiagLotes, # analizar un cuadro: que dibujos de autos se pueden juntar (<etiqueta>_lotes.txt)
    [switch]$AB,        # fotos A/B en la misma corrida: sin juntar (_A.bmp) y juntando (_B.bmp)
    [int]$Marcos = 0,   # posiciones de piezas movidas: 1 = relevamiento, 2 = en paralelo
    [switch]$Reflejo,   # brillo del reflejo una vez por modelo (y no una por auto)
    [switch]$Todo,      # todas las mejoras: -Elegida -Lotes 2 -Marcos 2 -Reflejo
    [switch]$TodoNvidia, # todas las mejoras con el driver de NVIDIA (sin DXVK)
    [int]$Jugadores = 0, # plazas del servidor (0 = automatico: 2, o 400 en las escenas con NPC)
    [int]$Referencias = 0, # referencias del motor agregadas a las 3000 originales (-Todo: 30000)
    [switch]$Atajos,       # DirectSound sin leer el registro + VirtualProtect de SA-MP sin repetir (-Todo: si)
    [int]$Choques = -1,    # choques: 0 = original, 1 = rechazo por cajas, 2 = sombra/validacion (-Todo: 1)
    [switch]$SonidoHilo,   # el servicio de sonido (DirectSound) en otro nucleo (-Todo: si)
    [int]$Animacion = -1,  # animacion de personas: 0 = original, 1 = en paralelo, 2 = sombra/validacion (-Todo: 1)
    [int]$EfectosSonido = -1, # efectos: claves de sonido guardadas: 0 = original, 1 = si, 2 = sombra (-Todo: 1)
    [int]$Efectos = -1     # efectos: particulas en varios nucleos: 0 = original, 1 = si, 2 = sombra (-Todo: 1)
)

$ErrorActionPreference = 'Stop'
$codigo = $PSScriptRoot
$root = Split-Path -Parent $codigo
$lab = Join-Path $root 'lab\GTA San Andreas'
$res = Join-Path $root 'resultados'
$launcher = Join-Path $codigo 'bin\lanzar_lab.exe'
$srv = Join-Path $root 'servidor_omp\Server'

if ($Todo -or $TodoNvidia) {
    if ($Todo) { $Elegida = $true }
    if ($TodoNvidia) { $Hilo = $true; if ($RuedasM -eq 0) { $RuedasM = 150 } }
    if ($Lotes -eq 0) { $Lotes = 2 }
    if ($Marcos -eq 0) { $Marcos = 2 }
    $Reflejo = $true
    if ($Referencias -eq 0) { $Referencias = 30000 }
    $Atajos = $true
    if ($Choques -lt 0) { $Choques = 1 }
    $SonidoHilo = $true
    if ($EfectosSonido -lt 0) { $EfectosSonido = 1 }
    if ($Efectos -lt 0) { $Efectos = 1 }
    if ($Animacion -lt 0) { $Animacion = 1 }
}
if ($Elegida) {
    $Hilo = $true
    $Dxvk = $true
    if ($RuedasM -eq 0) { $RuedasM = 150 }
}
# Las pausas (p. ej. la prueba de foco) no cuentan como medicion: dar tiempo de sobra antes de cortar
$Limite = [Math]::Max($Limite, $Espera + $Medicion + 150 + $(if ($Foco -gt 0) { [int]($Medicion / $Foco + 1) * 20 } else { 0 }))

# El plugin lee estas variables (tienen prioridad sobre samc.ini)
$env:MOTOR_SA_ETIQUETA = $Etiqueta
$env:MOTOR_SA_ESPERA = "$Espera"
$env:MOTOR_SA_MEDICION = "$Medicion"
$env:MOTOR_SA_MUESTREO = if ($MuestreoHilo) { '2' } elseif ($Muestreo) { '1' } else { '0' }   # 2 = muestrear el hilo de dibujo
$env:MOTOR_SA_VEHICULOS = '0'
$env:MOTOR_SA_D3D = if ($D3D -or $Hilo) { '1' } else { '0' }   # intermediario de Direct3D
$env:MOTOR_SA_HILO = if ($Hilo) { '1' } else { '0' }   # fila de dibujo en otro nucleo
$env:MOTOR_SA_RUEDAS_M = "$RuedasM"   # 0 = las ruedas se dibujan a cualquier distancia (como el original)
# Direct3D 9 sobre Vulkan (DXVK) en vez del driver Direct3D 9 de NVIDIA (solo laboratorio)
if ($Dxvk) {
    $env:MOTOR_SA_D3D = '1'
    $env:MOTOR_SA_D3D9_DLL = 'dxvk\d3d9.dll'
    $env:DXVK_LOG_LEVEL = 'info'
    $env:DXVK_LOG_PATH = Join-Path $root 'resultados'
} else {
    $env:MOTOR_SA_D3D9_DLL = '-'   # el Direct3D 9 del sistema
}
$env:MOTOR_SA_PRUEBA_FOCO = "$Foco"
$env:MOTOR_SA_LOTES = "$Lotes"
$env:MOTOR_SA_DIAG_LOTES = if ($DiagLotes) { '1' } else { '' }
$env:MOTOR_SA_AB = if ($AB) { '1' } else { '' }
$env:MOTOR_SA_MARCOS = "$Marcos"
$env:MOTOR_SA_REFLEJO = if ($Reflejo) { '1' } else { '0' }
$env:MOTOR_SA_REFERENCIAS = "$Referencias"
$env:MOTOR_SA_ATAJO_SONIDO = if ($Atajos) { '1' } else { '0' }
$env:MOTOR_SA_ATAJO_SAMP = if ($Atajos) { '1' } else { '0' }
$env:MOTOR_SA_CHOQUES = if ($Choques -gt 0) { "$Choques" } else { '0' }
$env:MOTOR_SA_SONIDO_HILO = if ($SonidoHilo) { '1' } else { '0' }
$env:MOTOR_SA_ANIMACION = if ($Animacion -gt 0) { "$Animacion" } else { '0' }
$env:MOTOR_SA_EFECTOS_SONIDO = if ($EfectosSonido -gt 0) { "$EfectosSonido" } else { '0' }
$env:MOTOR_SA_EFECTOS = if ($Efectos -gt 0) { "$Efectos" } else { '0' }

$resumen = Join-Path $res "${Etiqueta}_resumen.txt"
if (Test-Path -LiteralPath $resumen) { Remove-Item -LiteralPath $resumen }

$server = $null
if ($Modo -eq 'samp') {
    # Servidor local: solo 127.0.0.1, sin anunciarse en internet, con la escena elegida
    $orig = Join-Path $srv 'config.original.json'
    if (-not (Test-Path -LiteralPath $orig)) { Copy-Item -LiteralPath (Join-Path $srv 'config.json') -Destination $orig }
    $cfg = Get-Content -LiteralPath $orig -Raw | ConvertFrom-Json
    $cfg.announce = $false
    $cfg.name = 'SA:MC laboratorio'
    $cfg.max_players = if ($Jugadores -gt 0) { $Jugadores } elseif (($Escena -like 'bench_npc*' -or $Escena -like 'bench_combinado*')) { 400 } else { 2 }
    $cfg.max_bots = if (($Escena -like 'bench_npc*' -or $Escena -like 'bench_combinado*')) { $cfg.max_players - 2 } else { 0 }
    $cfg.artwork.enable = $false
    $cfg.network.bind = '127.0.0.1'
    $cfg.network.stream_radius = [double]$Radio
    if (-not (Test-Path -LiteralPath (Join-Path $srv "gamemodes\$Escena.amx"))) { throw "No existe la escena $Escena (compila con servidor\compilar.ps1)" }
    $cfg.pawn.main_scripts = @("$Escena 1")
    $cfg.game.use_nametags = $false
    $cfg.game.use_entry_exit_markers = $false
    $cfg.game.use_stunt_bonuses = $false
    $cfg.game.player_marker_mode = 0
    $cfg.game.weather = 1
    $cfg.game.time = 12
    # ConvertTo-Json escribe 400.0 como 400; open.mp ignora un entero donde espera decimal: forzar ".0"
    $json = [regex]::Replace(($cfg | ConvertTo-Json -Depth 10), '("(?:stream_radius|chat_radius|nametag_draw_radius|player_marker_draw_radius|minimum_send_bits_per_second|sleep|gravity)":\s*)(-?\d+)(\s*[,\r\n}])', '$1$2.0$3')
    [IO.File]::WriteAllText((Join-Path $srv 'config.json'), $json,
        (New-Object Text.UTF8Encoding($false)))

    $log = Join-Path $srv 'log.txt'
    if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log }
    $server = Start-Process -FilePath (Join-Path $srv 'omp-server.exe') -WorkingDirectory $srv -WindowStyle Hidden -PassThru
    $ok = $false
    for ($i = 0; $i -lt 80 -and -not $server.HasExited; $i++) {
        Start-Sleep -Milliseconds 250
        if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern '\[bench\] vehiculos=' -Quiet)) {
            $ok = $true
            break
        }
    }
    if (-not $ok) {
        'El servidor no arranco bien:'
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Tail 20 }
        if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
        exit 1
    }
    # Empezar a medir cuando el juego tenga cargado el 80% de los autos que puede recibir (máximo 700)
    $n = [int](Select-String -LiteralPath $log -Pattern 'vehiculos=(\d+)').Matches[0].Groups[1].Value
    $env:MOTOR_SA_VEHICULOS = [string][int]([math]::Min($n, 700) * 0.8)
    "Servidor listo (escena $Escena, $n autos, radio $Radio m)"
}

$argv = @('--limite', "$Limite")
if ($Modo -eq 'sp') {
    $argv += @('--sin-samp', $lab)
} else {
    $argv += @($lab, '-c', '-h', '127.0.0.1', '-p', '7777', '-n', 'Lab_Bench')
}

try {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $launcher @argv
    "Juego cerrado (codigo $LASTEXITCODE) tras $([int]$sw.Elapsed.TotalSeconds) s"
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $server.WaitForExit(10000) | Out-Null   # que suelte su archivo de registro antes de la próxima prueba
        'Servidor detenido'
    }
}

if (Test-Path -LiteralPath $resumen) {
    Get-Content -LiteralPath $resumen
} else {
    "No hay resumen: revisa $(Join-Path $lab 'samc.log')"
    if (Test-Path -LiteralPath (Join-Path $lab 'samc.log')) { Get-Content -LiteralPath (Join-Path $lab 'samc.log') -Tail 30 }
}
