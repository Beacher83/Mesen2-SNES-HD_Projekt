@echo off
REM ---------------------------------------------------------------------
REM  S49 A/B: Mesen MIT dem alten Verhalten starten.
REM
REM  Setzt SNES_HD_NO_OAM_TIEBREAK=1 -- damit verwirft das Franse-Tor jeden
REM  Prioritaets-Gleichstand wieder, genau wie vor S49. Zum Vergleichen
REM  einfach Mesen normal starten (dann ist der Tiebreak an).
REM
REM  Worauf zu achten ist, in snes_hd_diag.txt:
REM     sprFrTie      -- Gleichstaende, die verworfen wurden
REM     sprFrTieWon   -- Gleichstaende, die der OAM-Reihenfolge nach
REM                      GEZEICHNET wurden (mit diesem Schalter: 0)
REM  Das Banner listet den aktiven Schalter unter "A/B:".
REM
REM  Bildtest: Kleevers zerfallendes Schwert und Dixie mit der Kiste ueber
REM  dem Kopf -- dort ueberlappen gleichrangige Sprites.
REM ---------------------------------------------------------------------

set SNES_HD_NO_OAM_TIEBREAK=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
