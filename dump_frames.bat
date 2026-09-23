@echo off
REM ---------------------------------------------------------------------
REM  S48 Framebuffer-Dump — startet Mesen so, dass es zwei Bilder von
REM  Barrel Bayou (gfxset 38) nach Downloads schreibt.
REM
REM  Warum es diese Datei gibt: F12 wird auf diesem Rechner abgefangen,
REM  und ein Fensterfoto taugt nicht zum Messen (krummer Massstab
REM  verwischt das native Pixelraster). Hier kommt der exakte
REM  Ausgabepuffer des Filters heraus, 1024x896.
REM
REM  Ergebnis: %USERPROFILE%\Downloads\snes_hd_frame_gfx38_00.png (und _01)
REM ---------------------------------------------------------------------

set SNES_HD_DUMP_FRAMES=2
set SNES_HD_DUMP_GFXSET=38
set SNES_HD_DUMP_EVERY=30

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
