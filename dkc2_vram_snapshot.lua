--[[
  DKC2 VRAM-Schnappschuss — ein Level, ein Tastendruck, nichts wird ueberschrieben
  ==============================================================================

  Wozu: einen frischen VRAM-Abzug fuer EIN Level machen, um ihn im Viewer per
  VRAM-Symbol (importVramForSet) in ein Container-Set zu importieren.
  Anders als dkc2_vram_dump.lua (Sammel-Werkzeug fuer die Ground-Truth):
    - schreibt NICHT in den Ground-Truth-Ordner,
    - jede Aufnahme bekommt Datum+Uhrzeit im Namen, nichts wird ueberschrieben,
    - mehrere Aufnahmen im selben Level sind ausdruecklich erwuenscht
      (z.B. am Levelanfang und waehrend Kackle zu sehen ist).

  Anlass (28.09.): Der Ground-Truth-Abzug von Haunted Hall (gfxset 0x2C) steht
  unter Verdacht; Mesen erkannte das Level als gfxset 34. Ein frischer Abzug
  entscheidet es.

  EINRICHTEN:
  1. Mesen: Script > Script-Fenster > Einstellungen (Zahnrad)
     -> "Allow access to I/O and OS functions" einschalten
  2. DKC2 laden, Script > New Script Window, diese Datei oeffnen, Run.

  BEDIENEN:
  - Ins Level gehen. Oben links steht das gfxset aus WRAM $0539 (hex + dezimal).
    Haunted Hall muss "0x2C (44)" zeigen.
  - [End] druecken -> nach 3 Frames werden geschrieben:
      VRAM_G2C_<Datum>_<Zeit>.bin        64 KB VRAM   (DIESE in den Viewer)
      VRAM_G2C_<Datum>_<Zeit>_cgram.bin  512 B CGRAM
      VRAM_G2C_<Datum>_<Zeit>_info.txt   gfxset, Frame, Zeit
  - Ziel: %USERPROFILE%\Downloads. Klappt das nicht, der Script-Datenordner
    (der Pfad steht dann im Script-Log und im Bild).
]]--

local DUMP_KEY    = "End"   -- keine F-Taste: die kollidieren mit Mesens Hotkeys
local FRAMES_WAIT = 3       -- VBlank-DMA abwarten

local function outFolder()
  local home = os.getenv and os.getenv("USERPROFILE")
  if home then
    local dir = home .. "\\Downloads"
    local probe = io.open(dir .. "\\.dkc2_vram_probe", "w")
    if probe then
      probe:close()
      os.remove(dir .. "\\.dkc2_vram_probe")
      return dir
    end
  end
  return emu.getScriptDataFolder()
end
local folder = outFolder()
emu.log("DKC2 VRAM-Schnappschuss: Ziel = " .. folder)

local function readGfxset()
  return emu.read(0x0539, emu.memType.snesWorkRam)
end

local function dumpMem(path, memType)
  local f = io.open(path, "wb")
  if not f then return false end
  local size = emu.getMemorySize(memType)
  local parts = {}
  for i = 0, size - 1, 2 do
    local w = emu.read16(i, memType)
    parts[#parts + 1] = string.char(w % 256, math.floor(w / 256))
  end
  f:write(table.concat(parts))
  f:close()
  return true
end

local frame, keyWasDown, pending, waitCount = 0, false, false, 0
local lastMsg, lastMsgTimer, lastOk = "", 0, true

local function doDump()
  local g = readGfxset()
  local stamp = os.date("%Y%m%d_%H%M%S")
  local base = string.format("%s\\VRAM_G%02X_%s", folder, g, stamp)
  -- Zwei Aufnahmen in derselben Sekunde: Zaehler anhaengen statt ueberschreiben.
  local n = 1
  while true do
    local f = io.open(base .. ".bin", "rb")
    if not f then break end
    f:close()
    n = n + 1
    base = string.format("%s\\VRAM_G%02X_%s_%d", folder, g, stamp, n)
  end
  local okV = dumpMem(base .. ".bin", emu.memType.snesVideoRam)
  local okC = dumpMem(base .. "_cgram.bin", emu.memType.snesCgRam)
  local info = io.open(base .. "_info.txt", "w")
  if info then
    info:write(string.format("gfxset (WRAM $0539): 0x%02X (%d)\n", g, g))
    info:write(string.format("Zeit: %s\nFrame seit Scriptstart: %d\n", os.date("%Y-%m-%d %H:%M:%S"), frame))
    info:close()
  end
  lastOk = okV and okC
  local name = base:match("[^\\]+$")
  if lastOk then
    lastMsg = "OK: " .. name .. ".bin"
    emu.log("  geschrieben: " .. base .. ".bin (+ _cgram.bin, _info.txt)")
  else
    lastMsg = "FEHLER beim Schreiben nach " .. folder .. " (I/O im Script erlaubt?)"
    emu.log("  " .. lastMsg)
  end
  emu.displayMessage("VRAM", lastMsg)
  lastMsgTimer = 300
end

emu.addEventCallback(function()
  frame = frame + 1
  local g = readGfxset()

  local down = emu.isKeyPressed(DUMP_KEY)
  if down and not keyWasDown and not pending then
    pending, waitCount = true, 0
  end
  keyWasDown = down

  if pending then
    waitCount = waitCount + 1
    if waitCount >= FRAMES_WAIT then
      pending = false
      doDump()
    end
  end

  local bg, white, yellow, gray = 0xC0000000, 0xFFFFFF, 0xFFFF40, 0xA0A0A0
  emu.drawString(4, 4, string.format("VRAM-Schnappschuss  gfxset 0x%02X (%d)", g, g),
    g == 0x2C and 0x40FF40 or yellow, bg, 0, 1)
  emu.drawString(4, 14, pending and "schreibe..." or ("[" .. DUMP_KEY .. "] = Abzug"), gray, bg, 0, 1)
  if lastMsgTimer > 0 then
    lastMsgTimer = lastMsgTimer - 1
    emu.drawString(4, 24, lastMsg, lastOk and white or 0xFF4040, bg, 0, 1)
  end
end, emu.eventType.endFrame)
