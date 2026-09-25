@echo off
REM ---------------------------------------------------------------------
REM  S54 A/B: startet Mesen OHNE das Kandidatengitter -- der Umfaerber
REM  sucht wieder je Texel ueber alle 15 Farben (Verhalten bis S53b).
REM  Zum Vergleichen einfach Mesen.exe normal starten.
REM
REM  Testfall: Glimmer's Galleon mit Gegner im Bild. Das Bild muss in
REM  beiden Laeufen IDENTISCH sein, nur die Frame-Zeit (ms=cur/max in der
REM  FRAME-Zeile von snes_hd_diag.txt) soll ohne Schalter kleiner sein.
REM ---------------------------------------------------------------------

set SNES_HD_NO_RECOLOR_GRID=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
