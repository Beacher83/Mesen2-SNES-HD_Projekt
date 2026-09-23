@echo off
REM ---------------------------------------------------------------------
REM  S52 A/B: startet Mesen mit dem ALTEN Verhalten (R3-LUT statt
REM  Umfaerben je Farbe). Zum Vergleichen einfach Mesen.exe normal starten.
REM
REM  Testfall: Glimmer's Galleon. Mit dem Schalter muss das Negativ
REM  zurueckkommen, ohne ihn muss das Level dunkel und richtig aussehen.
REM  Frame-Zeit steht als ms=cur/max in der FRAME-Zeile von snes_hd_diag.txt.
REM ---------------------------------------------------------------------

set SNES_HD_NO_BG_RECOLOR=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
