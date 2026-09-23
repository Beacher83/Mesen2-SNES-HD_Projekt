@echo off
REM ---------------------------------------------------------------------
REM  S50 A/B -- Glimmer's Galleon wird mit HD-Pack als Negativ dargestellt,
REM  ohne Pack ist es korrekt. Diese Datei grenzt die Ursache ein.
REM
REM  SNES_HD_NO_SUB_HD_OPERAND=1
REM
REM  Ablauf: Glimmer's Galleon anspielen, hinsehen, mit dem normalen Start
REM  vergleichen. Der aktive Schalter steht im Sitzungsbanner unter "A/B:".
REM ---------------------------------------------------------------------

set SNES_HD_NO_SUB_HD_OPERAND=1

start "" "%~dp0bin\win-x64\Release\Mesen.exe"
