@echo off
REM ---------------------------------------------------------------------
REM  S47 Ebenen-Anstrich: jede BG-Ebene wird flaechig eingefaerbt --
REM  BG1 ROT, BG2 GRUEN, BG3 BLAU, BG4 GELB. Sprites bleiben unveraendert.
REM
REM  Testfall 25.09.: Luftstrom in Red Hot Ride. Welche Farbe hat er?
REM  Rot/Gruen/Blau = diese BG-Ebene. Bleibt er grau = er ist ein Sprite.
REM  Reiner Diagnosemodus -- zum Spielen normal Mesen.exe starten.
REM ---------------------------------------------------------------------

set SNES_HD_PAINT_LAYERS=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
