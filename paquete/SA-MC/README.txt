SA:MC 1.0 (San Andreas: MultiCore) - GTA San Andreas on all CPU cores
======================================================================
Creator: C0D3L0CK
(Espanol: ver LEEME.txt)

What it is
----------
A plugin (samc.asi) for GTA San Andreas 1.0 US that reworks parts of the game engine so heavy scenes
(many cars, many SA-MP players, many effects) run faster:

  - Rendering commands are handed to the graphics card from a second CPU core.
  - Identical cars are drawn in one batch (one instanced draw per model and part).
  - Car part positions are computed on several cores.
  - Raises engine limits that run out with many players (the "references" pool).
  - Removes repeated Windows calls made by the sound system (DirectSound) and by SA-MP.
  - Speeds up ped and car collisions (rejects early the tests the game would reject anyway).
  - Ped animation (the bone pose, the expensive part) is computed on several cores.
  - Sound (the 48 DirectSound channels) is handed over from another core.
  - Effects (fire, smoke, water): the game re-hashed 13 sound names per active effect every frame; now they
    are computed once. Particles are moved on several cores.
  - Optional: DXVK (Direct3D 9 on Vulkan), included.

Everything split across cores gives exactly the same result as the original game: it was checked byte by byte
(millions of animations, particles and sounds) against the original code.

It does not modify any game file. Remove it and the game is exactly as before.

Measured results (Xeon E5-2680 v4 + RTX 5060 Ti, 1920x1080, SA-MP 0.3.7, SA-MP FPS cap removed for testing)
-------------------------------------------------------------------------------------------------------------
  Scene                                    Original game    With SA:MC
  1000 cars at the airport                     44 FPS          134 FPS
  600 cars close to the camera                 43 FPS           96 FPS
  200 players walking and driving              33 FPS          151 FPS   (lows: 14 -> 98 FPS)
  500 fires and smoke effects                 197 FPS          453 FPS
  Everything at once: 200 players,
  300 cars and 100 effects                     27 FPS           91 FPS   (lows: 10 -> 66 FPS)

Note: SA-MP caps the frame rate (about 50 FPS with the menu frame limiter on, about 100 with it off). The gain
shows mostly when the game used to drop below that cap: full servers, events, races.

Requirements
------------
  - GTA San Andreas 1.0 US (the version used with SA-MP). With any other version the plugin does nothing.
  - An ASI loader. If you already use CLEO, modloader or other .asi files, you have one. If not, the installer
    offers SA:MC's own: a vorbisFile.dll that loads .asi files and forwards everything else to the original
    vorbisFile.dll (kept as vorbisHooked.dll; desinstalar.bat restores it if no other .asi needs it).
  - Windows 10 or 11. For DXVK: a Vulkan-capable graphics card.

Install
-------
  1. Close the game.
  2. Double-click instalar.bat. It finds the game folder (SA-MP stores it), checks the game version and the ASI
     loader, and copies samc.asi, samc.ini and the dxvk folder. If the game is in "Program Files",
     Windows asks for administrator permission.
  3. Play as usual (single player or SA-MP).

Manual install: copy the contents of the "archivos" folder into the game folder (where gta_sa.exe is).

Uninstall
---------
Double-click desinstalar.bat (or delete samc.asi, samc.ini and the dxvk folder from the game folder).

Options (samc.ini, open with Notepad; 1 = on, 0 = off)
----------------------------------------------------------
  hilo_dibujo=1            Render thread.
  d3d9_dll=dxvk\d3d9.dll   Use DXVK. Put a ; in front of the line to use the regular Direct3D 9.
  juntar_autos=2           Batched/instanced car drawing. 0 = off.
  ruedas_lejanas_m=150     No wheels on cars farther than 150 m (1-2 pixels). 0 = always draw wheels.
  piezas_paralelo=2        Car part positions on several cores. 0 = off.
  reflejo_una_vez=1        Reflection strength set once per car model (same result). 0 = off.
  referencias_extra=30000  More engine "references" (many players). 0 = original.
  atajo_sonido=1           DirectSound without registry reads on every sound update. 0 = off.
  atajo_samp=1             SA-MP memory-permission calls without repeats. 0 = off (e.g. if a client-side
                           anticheat complains).
  choques_rapidos=1        Faster collisions with identical results. 0 = original.
  sonido_hilo=1            Sound (DirectSound channel updates) handed over from another core. 0 = game core.
  animacion_paralelo=1     Ped animation on several cores (same result). 0 = original.
  efectos_sonido=1         Effect sound names hashed once (same result). 0 = original.
  efectos_paralelo=1       Effect particles on several cores (same result). 0 = original.
  registro_fps=1           Writes the FPS of every minute of play to samc.log. 0 = off.

Troubleshooting
---------------
  - Turn options off one at a time in samc.ini (start with d3d9_dll and juntar_autos).
  - samc.log tells what the plugin did. If the game is in "Program Files", Windows stores it in:
    %LOCALAPPDATA%\VirtualStore\Program Files (x86)\Rockstar Games\GTA San Andreas\samc.log
  - If you use ENB, ReShade or another d3d9.dll in the game folder: with SA:MC, rendering goes through DXVK.
    To keep yours, set d3d9_dll=d3d9.dll (no DXVK).

Licenses and credits: see LICENCIAS.txt.
