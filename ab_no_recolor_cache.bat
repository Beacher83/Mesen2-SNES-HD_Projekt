@echo off
REM ---------------------------------------------------------------------
REM  S57 A/B: startet Mesen OHNE den Kachel-Cache des Umfaerbens -- jede
REM  Kachel wird wieder in jedem Frame Texel fuer Texel umgefaerbt
REM  (Verhalten bis S56). Zum Vergleichen Mesen.exe normal starten.
REM
REM  Testfall: Screech's Sprint (gfxset 39, Referenz aus Bramble Blast).
REM  Das Bild muss in beiden Laeufen IDENTISCH sein. In snes_hd_diag.txt
REM  soll die Frame-Zeit (ms=cur/max) mit Cache deutlich kleiner sein;
REM  rcHit/rcMiss zeigen, wie oft der Cache traf (ohne Cache beide 0).
REM ---------------------------------------------------------------------

set SNES_HD_NO_RECOLOR_CACHE=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
