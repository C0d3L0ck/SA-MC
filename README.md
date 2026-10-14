# SA:MC — San Andreas: MultiCore

**Creador: C0D3L0CK**

**GTA San Andreas y SA-MP usando todos los núcleos del procesador.** SA:MC es un complemento (`samc.asi`) para
**GTA San Andreas 1.0 US** que reparte el trabajo del juego entre varios núcleos, en un jugador y en **SA-MP 0.3.7**.
Todo lo que se reparte da exactamente el mismo resultado que el juego original: se comprobó byte a byte contra el
código original del juego.

> **Mod no oficial.** Sin relación con Rockstar Games, Take-Two Interactive ni el equipo de SA-MP. Este repositorio
> no incluye ningún archivo del juego: hace falta una copia legal propia de GTA San Andreas.

*(English below.)*

## Qué hace

| Parte | Qué cambia | Opción en `samc.ini` |
|---|---|---|
| Dibujo | Las órdenes para la tarjeta de video se entregan desde otro núcleo | `hilo_dibujo=1` |
| DXVK | Direct3D 9 sobre Vulkan (opcional) | `d3d9_dll=dxvk\d3d9.dll` |
| Autos | Autos iguales dibujados en una sola orden ("en bola") | `juntar_autos=2` |
| Autos | Posiciones de las piezas calculadas en varios núcleos | `piezas_paralelo=2` |
| Autos | Sin ruedas a más de 150 m (miden 1 o 2 píxeles) | `ruedas_lejanas_m=150` |
| Autos | Brillo del reflejo una vez por modelo (mismo resultado) | `reflejo_una_vez=1` |
| Personas | La pose de los huesos se calcula en varios núcleos | `animacion_paralelo=1` |
| Efectos | Partículas de fuego, humo y agua en varios núcleos | `efectos_paralelo=1` |
| Efectos | Los nombres de 13 sonidos se calculan una vez y no en cada cuadro | `efectos_sonido=1` |
| Sonido | Los 48 canales de DirectSound se atienden desde otro núcleo | `sonido_hilo=1` |
| Choques | Descarta antes las pruebas que el juego igual rechazaba (mismo resultado) | `choques_rapidos=1` |
| SA-MP | 30 000 "referencias" más (con muchos jugadores se agotaban las 3000 del juego) | `referencias_extra=30000` |
| SA-MP | Sin llamadas repetidas a Windows (registro de DirectSound y permisos de memoria) | `atajo_sonido=1`, `atajo_samp=1` |
| Diagnóstico | Anota los FPS de cada minuto de juego en `samc.log` | `registro_fps=1` |

Los choques y el control de cada persona y auto siguen en el núcleo principal: cada choque cambia a los dos que
chocan, así que repartirlos cambiaría cómo se comporta el juego. El tope de FPS de SA-MP no se toca.

## Resultados

Xeon E5-2680 v4 (14 núcleos) + RTX 5060 Ti, 1920x1080, SA-MP 0.3.7 contra un servidor open.mp local, medido en
laboratorio sin el tope de FPS de SA-MP. Entre paréntesis, el 1 % de cuadros más lentos.

| Escena | Juego original | Con SA:MC |
|---|---|---|
| 1000 autos en el aeropuerto | 44 FPS | 134 FPS |
| 600 autos cerca de la cámara | 43 FPS | 96 FPS |
| 200 jugadores caminando y manejando | 33 FPS (14) | 151 FPS (98) |
| 500 fuegos y humos | 197 FPS | 453 FPS |
| Todo junto: 200 jugadores, 300 autos y 100 efectos | 27 FPS (10) | 91 FPS (66) |

En servidores reales SA-MP mantiene su tope (unos 100 FPS con el limitador del menú apagado, unos 50 encendido):
la mejora se nota cuando el juego bajaba de ese tope (servidores llenos, eventos, carreras).

## Instalar

1. Descarga `SA-MC_1.0.zip` de la sección **Releases** y descomprímelo.
2. Doble clic en `instalar.bat` (busca la carpeta del juego y el cargador de .asi, y copia `samc.asi`,
   `samc.ini` y `dxvk\d3d9.dll`).
3. Juega como siempre. Para quitarlo: `desinstalar.bat`.

Si el juego se cierra al conectarte a cualquier servidor de SA-MP, revisa los scripts de CLEO que cambian la skin
del personaje al iniciar: pueden chocar con SA-MP (pasa también sin SA:MC).

## Compilar

- Windows y PowerShell.
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (ucrt, x86_64) en
  `herramientas\llvm-mingw-20261006-ucrt-x86_64\`, o la carpeta `bin` del compilador en la variable `MOTOR_SA_LLVM`.
- MinHook 1.3.4 ya viene en `terceros\`.

```
powershell -ExecutionPolicy Bypass -File codigo\build.ps1 -Publico
```

Deja `codigo\bin\publico\samc.asi` (la versión para repartir) y `codigo\bin\publico\vorbisFile.dll` (cargador de
.asi propio). Para armar el zip, copia esos dos archivos a `paquete\SA-MC\archivos\` (el cargador en `cargador\`) y
agrega `dxvk\d3d9.dll` de [DXVK](https://github.com/doitsujin/dxvk) 3.1.1 (32 bits).

## Carpetas

| Carpeta | Qué hay |
|---|---|
| `codigo\src\` | El complemento |
| `codigo\cargador\` | Cargador de .asi (`vorbisFile.dll`) |
| `codigo\launcher\`, `codigo\prueba.ps1` | Laboratorio: abre una copia del juego, mide y la cierra |
| `codigo\servidor\` | Escenas de prueba para un servidor open.mp local |
| `paquete\SA-MC\` | Instalador, desinstalador, textos y `samc.ini` |
| `docs\LABORATORIO.md` | Cómo se midió y qué hace cada pieza |
| `terceros\minhook-1.3.4\` | MinHook (licencia BSD de 2 cláusulas) |

## Licencia

Código propio: licencia MIT, Copyright (c) 2026 C0D3L0CK (ver `LICENSE`). Terceros: MinHook (BSD de 2 cláusulas) y
DXVK (zlib/libpng); sus avisos están en `paquete\SA-MC\LICENCIAS.txt`.

---

# SA:MC — San Andreas: MultiCore (English)

**Creator: C0D3L0CK**

**GTA San Andreas and SA-MP on all your CPU cores.** SA:MC is a plugin (`samc.asi`) for **GTA San Andreas
1.0 US** that spreads the game's work across several CPU cores, in single player and in **SA-MP 0.3.7**. Everything
that is split across cores gives exactly the same result as the original game: it was checked byte by byte against
the game's original code.

> **Unofficial mod.** Not affiliated with Rockstar Games, Take-Two Interactive or the SA-MP team. This repository
> contains no game files: you need your own legal copy of GTA San Andreas.

## What it does

- Rendering commands are handed to the graphics card from another core (optional DXVK: Direct3D 9 on Vulkan).
- Identical cars are drawn in one instanced draw; car part positions are computed on several cores; no wheels
  beyond 150 m.
- Ped animation (the bone pose) is computed on several cores.
- Effect particles (fire, smoke, water) are moved on several cores; 13 effect sound names are hashed once instead of
  every frame.
- Sound (the 48 DirectSound channels) is serviced from another core.
- Faster collisions with identical results; 30,000 extra engine "references" for crowded SA-MP servers; no repeated
  Windows calls from DirectSound and SA-MP.
- `registro_fps=1` writes the FPS of every minute of play to `samc.log`.

Collisions and per-ped/per-car control stay on the main core: each collision changes both bodies, so splitting it
would change how the game behaves. The SA-MP FPS cap is not touched.

## Results

Same lab and table as above: 1000 cars 44 → 134 FPS, 600 near cars 43 → 96, 200 players 33 → 151 (lows 14 → 98),
500 effects 197 → 453, everything at once 27 → 91 (lows 10 → 66). On real servers SA-MP keeps its cap (~100 FPS with
the menu frame limiter off, ~50 with it on); the gain shows where the game used to drop below it.

## Install

Download `SA-MC_1.0.zip` from **Releases**, unzip it and double-click `instalar.bat`. To remove it:
`desinstalar.bat`. If the game closes when joining any SA-MP server, check CLEO scripts that change the player skin
at startup (it also happens without SA:MC).

## Build

Windows, PowerShell, [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (ucrt, x86_64) in
`herramientas\llvm-mingw-20261006-ucrt-x86_64\` (or its `bin` folder in `MOTOR_SA_LLVM`). MinHook 1.3.4 is in
`terceros\`. Run `powershell -ExecutionPolicy Bypass -File codigo\build.ps1 -Publico`.

## License

Own code: MIT License, Copyright (c) 2026 C0D3L0CK (see `LICENSE`). Third parties: MinHook (BSD 2-clause) and DXVK
(zlib/libpng); their notices are in `paquete\SA-MC\LICENCIAS.txt`.
