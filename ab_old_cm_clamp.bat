@echo off
REM ---------------------------------------------------------------------
REM  S55 A/B: startet Mesen mit der ALTEN Reihenfolge der Farbmathematik
REM  (erst kappen, dann halbieren). Zum Vergleichen Mesen.exe normal starten.
REM
REM  Testfall 25.09.: Dampf in Red Hot Ride. Mit dem Schalter muss er wieder
REM  deckend grau sein, ohne ihn halbtransparent weiss wie ohne HD-Pack.
REM ---------------------------------------------------------------------

set SNES_HD_OLD_CM_CLAMP=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
