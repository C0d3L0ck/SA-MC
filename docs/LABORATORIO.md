# SA:MC (San Andreas: MultiCore, antes Motor SA): San Andreas usando varios núcleos

Proyecto para reprogramar partes del motor de GTA San Andreas (1.0 US, compatible con SA-MP 0.3.7)
para que las escenas pesadas, como 100 vehículos en pantalla, repartan trabajo entre varios núcleos.

El juego real (`C:\Program Files (x86)\Rockstar Games\GTA San Andreas`) **no se toca**. Todo se prueba
en una copia de laboratorio. Solo cuando una pieza sea estable y mejore los números, pasa al juego real.

## Carpetas

| Carpeta | Qué hay |
|---|---|
| `codigo\` | Nuestro código: el plugin `samc.asi`, el lanzador del laboratorio y los scripts |
| `codigo\bin\` | Lo compilado (`samc.asi`, `lanzar_lab.exe`) |
| `lab\GTA San Andreas\` | Copia del juego para pruebas (sin CLEO ni modloader) |
| `lab\UserFiles\` | Configuración y partidas propias del laboratorio (no usa tus Documentos) |
| `resultados\` | Mediciones de cada prueba (`*_resumen.txt`, `*_cuadros.csv`) |
| `herramientas\` | Compilador llvm-mingw 20261006 (descargado de GitHub, huella verificada) |
| `terceros\` | MinHook 1.3.4 (para enganchar nuestro código a las funciones del juego) |
| `descargas\` | Los archivos originales descargados |

## Fases

1. **Laboratorio**: copia del juego y prueba automática (abre, espera, mide, cierra).
2. **Medidor**: cronómetro en cada parte del motor; gráfico de en qué se va cada milisegundo.
3. **Reprogramar por piezas**: equipo de trabajadores (un hilo por núcleo libre) y repartir la pieza
   más pesada; comprobar que el resultado sea idéntico al original y medir la ganancia.
4. **SA-MP**: prueba en un servidor real con muchos vehículos; después, al juego real.

## Cómo compilar y probar

```
powershell -ExecutionPolicy Bypass -File codigo\build.ps1 -Instalar
powershell -ExecutionPolicy Bypass -File codigo\prueba.ps1 -Etiqueta base_sp -Modo sp
```

El juego se abre en pantalla, espera a que cargue, mide y se cierra solo. Si algo falla, la causa
queda anotada en `lab\GTA San Andreas\samc.log`.

## Qué hace hoy el plugin (laboratorio)

| Pieza | Qué hace | Opción |
|---|---|---|
| Fila de dibujo | Las órdenes para la tarjeta de video se entregan desde otro núcleo | `hilo_dibujo=1` / `-Hilo` |
| DXVK | Direct3D 9 sobre Vulkan como "traductor" (opcional; con el dibujo en bola ya no hace falta para la velocidad) | `d3d9_dll=dxvk\d3d9.dll` / `-Dxvk` |
| Ruedas lejanas | No dibuja ruedas de autos a más de N metros (casi invisible) | `ruedas_lejanas_m=150` / `-RuedasM 150` |
| Juntador de autos | Agrupa los dibujos iguales de autos (1) y los dibuja "en bola", una orden por grupo (2) | `juntar_autos=2` / `-Lotes 2` |

| Piezas en paralelo | Las posiciones de las piezas de los autos se calculan en varios núcleos | `piezas_paralelo=2` / `-Marcos 2` |
| Reflejo una vez | El brillo del reflejo se aplica una vez por modelo y no una por auto (mismo resultado) | `reflejo_una_vez=1` / `-Reflejo` |
| Referencias | 30 000 "referencias" más (el juego trae 3000 y con ~200 jugadores se agotan: un tercio del cuadro buscándolas) | `referencias_extra=30000` / `-Referencias N` |
| Atajos | DirectSound sin leer el registro en cada sonido; VirtualProtect de SA-MP sin repetir | `atajo_sonido=1`, `atajo_samp=1` / `-Atajos` |
| Choques rápidos | Rechazo por cajas antes de TestSphereTriangle y réplica exacta de GetIsTouching (2 = modo sombra: compara) | `choques_rapidos=1` / `-Choques N` |
| Sonido en otro hilo | CAEAudioHardware::Service (los 48 canales de DirectSound) en un hilo propio, con esperas en cada entrada al hardware de audio | `sonido_hilo=1` / `-SonidoHilo` |
| Animación en paralelo | Bucle propio de animaciones: envoltura, A y B en orden en el principal; la pose de los huesos (C) de cada tanda repartida; D y E en orden. 0xB4EA0C por hilo (TLS). En serie lo que puede borrar o descomprimir animaciones; un callback de fin cierra la tanda | `animacion_paralelo=1` / `-Animacion N` |
| Efectos: sonido | FxSystem_c::DoFxAudio con las 13 claves de sonido calculadas una vez (antes: 62 000 llamadas al CRT por cuadro con 500 efectos) | `efectos_sonido=1` / `-EfectosSonido N` |
| Efectos en paralelo | Partículas de los emisores "puros" (FORCE, FRICTION, WIND, ROTSPEED) en varios núcleos; las muertas vuelven a la lista libre en el orden original | `efectos_paralelo=1` / `-Efectos N` |

En las tres últimas, 2 = modo sombra: hace lo repartido, lo deshace, corre el original y compara byte a byte (el juego
sigue con el resultado original). Resultado: 0 diferencias en 2,4 millones de animaciones (un jugador, 200 NPC y la
escena combinada), 18 600 cuadros de partículas y 7,6 millones de llamadas de sonido.

`prueba.ps1 -Elegida` = fila de dibujo + DXVK + ruedas a 150 m. `prueba.ps1 -Todo` = todo lo anterior + juntar autos
en bola + piezas en paralelo + reflejo una vez + referencias + atajos + choques + sonido en otro hilo + animación y
efectos en paralelo (`-TodoNvidia`: lo mismo sin DXVK).

El chat de SA-MP (D3DX) bloquea su búfer entero con NOOVERWRITE y escribe solo un trozo: la fila guarda un espejo de
cada búfer dinámico y sube solo lo escrito (páginas vistas con MEM_WRITE_WATCH). Así el chat queda igual al original.

## Paquete para repartir

`build.ps1 -Publico` compila `bin\publico\samc.asi` (sin nada del laboratorio: ni la prueba
automática; solo obedece a `samc.ini`, sección `[samc]`) y `bin\publico\vorbisFile.dll` (cargador de
.asi propio que reenvía las 25 funciones de Ogg Vorbis a la original renombrada `vorbisHooked.dll`).
`paquete\SA-MC\` (y `SA-MC_1.0.zip`): `instalar.bat` / `desinstalar.bat` (buscan la carpeta del juego,
piden administrador si hace falta, respaldan versiones anteriores, ponen el cargador solo si no hay uno),
`archivos\` (lo que va a la carpeta del juego), LEEME/README/LICENCIAS. No incluye ningún archivo del juego.
Para probar los scripts sin tocar nada: `-Carpeta <copia> -SinPausa -SinElevar`.

## Escenas de prueba (servidor open.mp local, `codigo\servidor\`)

| Escena | Qué es |
|---|---|
| `bench_1000` | 1000 autos en la pista del aeropuerto, cámara mirando a lo largo (autos de 20 a 350 m) |
| `bench_cerca` | La misma pista con 30 modelos y colores variados, cámara al costado: ~160 autos a menos de 115 m |
| `bench_paseo` / `bench_paseo_rapido` | Los 1000 autos y un paseo por Los Santos (prueba de estabilidad y memoria) |
| `bench_npc` | 200 jugadores manejados por el servidor (NPC de open.mp): 150 caminan/corren y 50 manejan autos |
| `bench_gente` | 300 actores animados (el cliente de SA-MP carga ~51) |
| `bench_efectos` / `bench_efectos500` | 120 / 500 objetos de partículas de SA-MP (fuego, humo, agua) |
| `bench_humo` | `bench_cerca` con los autos dañados y el motor encendido |
| `bench_combinado` | Todo junto: 150 NPC caminando, 50 manejando, 300 autos estacionados y 100 efectos |

| Escena (`-Todo`, Xeon E5-2680 v4 + RTX 5060 Ti) | Original | Ahora |
|---|---|---|
| `bench_1000` | 44 FPS | 134 FPS |
| `bench_cerca` | 43 FPS | 96 FPS |
| `bench_npc` | 33 FPS (1 % bajo 14) | 151 FPS (1 % bajo 98) |
| `bench_efectos500` | 197 FPS | 453 FPS (1 % bajo 370) |
| `bench_combinado` | 27 FPS (1 % bajo 10) | 91 FPS (1 % bajo 66) |

Lo que queda en el hilo principal en `bench_combinado` (perfil `comb_perfil`): choques (ProcessCollisionSectorList
≈20 %: cada choque cambia la velocidad de los dos implicados, el orden importa) y ProcessControl de cada persona y auto
(≈16 %), más el armado del dibujo de RenderWare (≈35 %, ya entregado a la fila de dibujo).

## Herramientas de comprobación

- `prueba.ps1 -AB`: dos fotos del mismo momento, sin juntar (`_A.bmp`) y juntando (`_B.bmp`).
- `codigo\comparar_capturas.py` y `codigo\zonas_distintas.py` (con el Python de Blender): diferencias entre fotos.
- `prueba.ps1 -Foco 30`: minimiza y restaura el juego cada 30 s (como Alt+Tab) y saca fotos al volver.
- `<etiqueta>_memoria.csv` y `codigo\comparar_memoria.py`: memoria cada 5 s.
- `prueba.ps1 -Muestreo` / `-MuestreoHilo` y `codigo\simbolos.py`: en qué se va el tiempo de cada hilo. El informe
  trae además tiempo inclusivo por función del juego, árboles de llamadas (CWorld::Process, audio) y las cadenas de
  llamadas que terminan dentro de Windows (ntdll).
- `MOTOR_SA_DIAG_LLAMADAS=1`: registro de las llamadas a Direct3D (con el módulo que llama) alrededor de los dibujos
  en otras superficies, en `lab\GTA San Andreas\samc_llamadas.txt`.
- `<etiqueta>_limites.txt`: referencias en uso, llamadas evitadas por los atajos y cuentas de los choques.
