--[[
  DKC2 Framebuffer-Screenshot — Mesen2 Lua Script
  ================================================

  Speichert den EXAKTEN Ausgabepuffer des HD-Filters als PNG.

  WARUM DIESES SKRIPT EXISTIERT (23.09.2026):
  Ein Screenshot vom Fenster taugt nicht zum Messen. Das Fensterfoto vom
  21.09. war 1071 px breit fuer 256 native Pixel — Massstab 4,1836. Diese
  krumme Skalierung verwischt genau das native Pixelraster, an dem man
  nativ von HD unterscheidet, und hat einen halben Tag Fehlanalyse
  gekostet. Mesens eigene Screenshot-Taste (F12) haette das geloest, wird
  auf diesem Rechner aber von etwas anderem abgefangen.

  emu.takeScreenshot() geht denselben Weg wie F12 (LuaApi.cpp:808 ->
  VideoDecoder -> BaseVideoFilter::TakeScreenshot) und liefert den Puffer
  des Videofilters, also bei aktivem HD-Pack das 4x-Bild — ganzzahliger
  Massstab, 1024x896, nichts nachbearbeitet.

  Geprueft am 23.09.: VideoFilter=None, ScanlineIntensity=0,
  ScreenRotation=None, alle Farbregler 0. Es kommt also wirklich der rohe
  Puffer heraus. Sollte sich das aendern, misst dieses Skript wieder etwas
  anderes als den Emulatorausgang.

  SETUP:
  1. Mesen: Script > Script Window > Settings (Zahnrad)
     -> "Allow access to I/O and OS functions" einschalten
  2. Script > New Script Window, diese Datei laden

  BEDIENUNG:
  - [Home] speichert einen Screenshot
  - Ziel: %USERPROFILE%\Downloads\snes_hd_shot_NN.png (durchnummeriert,
    nichts wird ueberschrieben)
  - Jeder Treffer landet auch im Script-Log mit Pfad und Bildgroesse
]]--

------------------------------------------------------------------------
-- Konfiguration
------------------------------------------------------------------------
-- Bewusst KEINE F-Taste: F12 wird auf diesem Rechner abgefangen, und
-- F1-F8 kollidieren mit Mesens Save/Load-State sowie mit den Tasten von
-- dkc2_vram_dump.lua (F2/F3/End/Page Up/Page Down).
local SHOT_KEY = "Home"

local PREFIX = "snes_hd_shot_"

------------------------------------------------------------------------

local function outputFolder()
  -- os.getenv braucht "Allow access to I/O and OS functions".
  local home = os.getenv("USERPROFILE")
  if home and home ~= "" then
    return home .. "\\Downloads\\"
  end
  -- Fallback, falls OS-Zugriff aus ist: dann schlaegt io.open fehl und
  -- das Skript sagt es im Log, statt still nichts zu tun.
  return "C:\\Users\\beach\\Downloads\\"
end

local folder = outputFolder()

--- Naechsten freien Dateinamen suchen, damit Vergleichslaeufe
--- (mit Pack / ohne Pack) sich nicht gegenseitig ueberschreiben.
local function nextFreePath()
  for i = 0, 999 do
    local path = string.format("%s%s%02d.png", folder, PREFIX, i)
    local f = io.open(path, "rb")
    if not f then
      return path
    end
    f:close()
  end
  return nil
end

local function saveShot()
  local png = emu.takeScreenshot()
  if not png or #png == 0 then
    emu.log("[shot] takeScreenshot() lieferte nichts — laeuft ein ROM?")
    return
  end

  local path = nextFreePath()
  if not path then
    emu.log("[shot] Keine freie Nummer mehr (000-999 belegt).")
    return
  end

  local f = io.open(path, "w+b")
  if not f then
    emu.log("[shot] Kann nicht schreiben: " .. path)
    emu.log("[shot] -> Script-Settings: 'Allow access to I/O and OS functions' einschalten.")
    return
  end
  f:write(png)
  f:close()

  emu.log(string.format("[shot] gespeichert: %s  (%d Bytes)", path, #png))
  emu.displayMessage("Screenshot", "Gespeichert: " .. string.match(path, "[^\\]+$"))
end

------------------------------------------------------------------------
-- Tastenabfrage: Flanke, nicht Zustand — sonst schreibt ein gehaltener
-- Tastendruck sechzig Dateien je Sekunde.
------------------------------------------------------------------------
local wasDown = false

emu.addEventCallback(function()
  local down = emu.isKeyPressed(SHOT_KEY)
  if down and not wasDown then
    saveShot()
  end
  wasDown = down
end, emu.eventType.endFrame)

emu.log("[shot] bereit — [" .. SHOT_KEY .. "] speichert nach " .. folder .. PREFIX .. "NN.png")
