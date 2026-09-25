@echo off
REM ---------------------------------------------------------------------
REM  S53b A/B: startet Mesen mit dem Paletten-Verhalten VOR S53 -- jede
REM  BG-Kachel nimmt wieder die 4bpp-Zeile pal*16, auch BG3 (2bpp).
REM  Zum Vergleichen einfach Mesen.exe normal starten.
REM
REM  Testfaelle (25.09.): Red Hot Ride (Luftstrom aus der Lava) und
REM  Rickety Race (Flagge). Sieht es MIT dem Schalter richtig aus, ist
REM  S53 die Ursache.
REM ---------------------------------------------------------------------

set SNES_HD_OLD_BG_PAL_ROWS=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
