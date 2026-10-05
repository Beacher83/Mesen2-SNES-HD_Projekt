@echo off
REM ---------------------------------------------------------------------
REM  S58 A/B: Mesen MIT dem alten Verhalten starten.
REM
REM  Setzt SNES_HD_NO_SEAM_GUARD=1 -- dann blendet ein weicher HD-Sprite-Rand
REM  auch MITTEN im Sprite gegen den Hintergrund, wie bis S57 (duenne
REM  transparente Linie an Diddys Sonnenbrille / Dixies Gitarre am Level-Ende).
REM  Zum Vergleichen einfach Mesen normal starten.
REM
REM  In snes_hd_diag.txt: sprSeam = Pixel, an denen S58 den Hintergrund als
REM  Untergrund abgelehnt hat (mit diesem Schalter: 0).
REM  Das Banner listet den aktiven Schalter unter "A/B:".
REM ---------------------------------------------------------------------

set SNES_HD_NO_SEAM_GUARD=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
