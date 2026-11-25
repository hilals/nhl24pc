NHL24 PC - ready-to-run package
================================
1. Keep this folder together (nhl24.exe, launcher.exe, *.dll, *.png, user\).
   Put it somewhere you can write to (not Program Files), path without special characters is safest.
2. Start launcher.exe. Choose "Install PKG" and pick your NHL 24 BASE GAME package (v01.00, title ID
   CUSA37933). The base game works as it is; the 1.60 update also runs with the patches in
   user\patches\nhl24.xml. The game is extracted into game\ next to the launcher.
3. Press PLAY. Settings (resolution, fullscreen, vsync, sharpening, controls) are in the launcher.
   Saves, profile and the shader cache are created under user\ on first run; the first minutes of
   play stutter while shaders compile, it gets smooth afterwards.
4. If the game crashes while a match loads (right after team selection, error 0xc0000005), turn on
   "Red zone protection (Windows)" in the launcher's quick settings and start the game again.

Requirements: Windows 10/11 x64, a Vulkan 1.3 GPU with current drivers (NVIDIA GTX 10xx+/RTX, AMD RDNA),
8 GB RAM (16 GB recommended), ~40 GB disk for the extracted game. A gamepad (Xbox/DualShock/DualSense)
works out of the box; keyboard mapping is in user\input_config\default.ini.

DLSS (RTX 20/30/40/50 cards): nvngx_dlss.dll and bbport_dlss.dll must stay next to nhl24.exe; pick the
quality mode and the output size in the launcher. The frame rate cap (60/90/120/144) and PS4 Pro mode
(experimental) are also set in the launcher.

Source, documentation and issues: https://github.com/hilals/nhl24pc (based on https://github.com/HajDEO/nhl24pc)

Slovensky: spustite launcher.exe, cez "Install PKG" vyberte PKG základnej hry NHL 24 v01.00 (základná hra
funguje; update 1.6 beží s patchmi v user\patches\nhl24.xml), hra sa
rozbalí do game\ vedľa launchera; potom PLAY. Uložené pozície a profil
vznikajú v user\ pri prvom spustení. Ak hra padá pri načítaní zápasu (hneď po výbere tímov), zapnite
v launcheri "Ochrana red zone (Windows)" a spustite hru znova.
