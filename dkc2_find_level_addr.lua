--[[
  DKC2 Level-ID / GfxSet Address Finder
  ======================================
  Hilft die korrekte WRAM-Adresse für die Level-ID oder den GfxSet-Index zu finden.

  USAGE:
  1. In Pirate Panic (Level 1) ins Level eintreten → [F4] drücken
  2. Zu Mainbrace Mayhem (Level 2) navigieren → eintreten → [F4] drücken
  3. Log zeigt alle veränderten WRAM-Bytes + Kandidaten-Adressen

  Bekannte GfxSet-Werte:
    Pirate Panic       = gfxset 0x07
    Mainbrace Mayhem   = gfxset 0x25
    Krow's Nest area   = gfxset 0x25
]]--

local SCAN_KEY = "F4"
local WRAM_SIZE = 0x2000   -- 8KB WRAM (Low-WRAM, $7E0000-$7E1FFF)

-- Bekannte Gfxset-Zielwerte (Level 1 und Level 2)
local TARGET_A = 0x07   -- Pirate Panic Gfxset
local TARGET_B = 0x25   -- Mainbrace Mayhem Gfxset

local snapshot1 = nil
local snapshot2 = nil
local keyWasDown = false

local function takeSnapshot()
  local snap = {}
  for addr = 0, WRAM_SIZE - 1 do
    snap[addr] = emu.read(addr, emu.memType.snesWorkRam)
  end
  return snap
end

local function analyze()
  emu.log("\n" .. string.rep("=", 60))
  emu.log("  WRAM-Vergleich: Snapshot 1 vs Snapshot 2")
  emu.log(string.rep("=", 60))

  local diffs = {}
  for addr = 0, WRAM_SIZE - 1 do
    if snapshot1[addr] ~= snapshot2[addr] then
      table.insert(diffs, {
        addr  = addr,
        val1  = snapshot1[addr],
        val2  = snapshot2[addr],
      })
    end
  end

  emu.log(string.format("Gesamt verändert: %d Adressen\n", #diffs))

  -- Kandidaten: Snap1 = TARGET_A (0x07) und Snap2 = TARGET_B (0x25)
  emu.log(string.rep("-", 60))
  emu.log(string.format(
    "KANDIDATEN: Snap1=%02X (Pirate Panic GfxSet) → Snap2=%02X (Mainbrace GfxSet)",
    TARGET_A, TARGET_B))
  emu.log(string.rep("-", 60))
  local candidates = 0
  for _, d in ipairs(diffs) do
    if d.val1 == TARGET_A and d.val2 == TARGET_B then
      emu.log(string.format("  *** $%04X: %02X → %02X  ← SEHR WAHRSCHEINLICH ***", d.addr, d.val1, d.val2))
      candidates = candidates + 1
    end
  end
  if candidates == 0 then
    emu.log("  (keine direkten Kandidaten — prüfe Alternativwerte unten)")
  end

  -- Alle Adressen die in Snap1 = 0x07 haben
  emu.log("\n" .. string.rep("-", 60))
  emu.log(string.format("Adressen mit Snap1=%02X (GfxSet Pirate Panic):", TARGET_A))
  emu.log(string.rep("-", 60))
  for _, d in ipairs(diffs) do
    if d.val1 == TARGET_A then
      emu.log(string.format("  $%04X: %02X → %02X", d.addr, d.val1, d.val2))
    end
  end

  -- Alle Adressen die in Snap2 = 0x25 haben
  emu.log("\n" .. string.rep("-", 60))
  emu.log(string.format("Adressen mit Snap2=%02X (GfxSet Mainbrace):", TARGET_B))
  emu.log(string.rep("-", 60))
  for _, d in ipairs(diffs) do
    if d.val2 == TARGET_B then
      emu.log(string.format("  $%04X: %02X → %02X", d.addr, d.val1, d.val2))
    end
  end

  -- Wert von $003E zum Vergleich
  emu.log("\n" .. string.rep("-", 60))
  emu.log(string.format("Referenz: $003E: Snap1=%02X  Snap2=%02X  (war die falsche Adresse)",
    snapshot1[0x003E], snapshot2[0x003E]))
  emu.log(string.rep("=", 60) .. "\n")
end

emu.addEventCallback(function()
  local keyDown = emu.isKeyPressed(SCAN_KEY)
  if keyDown and not keyWasDown then
    if not snapshot1 then
      snapshot1 = takeSnapshot()
      local current003E = snapshot1[0x003E]
      emu.log(string.format(
        "[Snapshot 1] WRAM gespeichert. $003E=%02X — jetzt zu Level 2 und erneut [%s]",
        current003E, SCAN_KEY))
      emu.displayMessage("WRAM Scan", "Snapshot 1 OK. Jetzt Level 2 betreten und [F4].")
    elseif not snapshot2 then
      snapshot2 = takeSnapshot()
      local current003E = snapshot2[0x003E]
      emu.log(string.format("[Snapshot 2] WRAM gespeichert. $003E=%02X — Analysiere...", current003E))
      emu.displayMessage("WRAM Scan", "Snapshot 2 OK. Analyse läuft...")
      analyze()
    else
      -- Reset für neuen Durchlauf
      snapshot1 = nil
      snapshot2 = nil
      emu.log("[Reset] Snapshots gelöscht. Erneut [F4] in Level 1 drücken.")
      emu.displayMessage("WRAM Scan", "Reset. F4 in Level 1 drücken.")
    end
  end
  keyWasDown = keyDown
end, emu.eventType.endFrame)

emu.log("================================================================")
emu.log("  DKC2 WRAM Level-Adress-Finder geladen")
emu.log("================================================================")
emu.log("  [F4] in Pirate Panic drücken  (Snap 1)")
emu.log("  [F4] in Mainbrace Mayhem drücken (Snap 2)")
emu.log("  → Log zeigt Kandidaten-Adressen")
emu.log("================================================================")
