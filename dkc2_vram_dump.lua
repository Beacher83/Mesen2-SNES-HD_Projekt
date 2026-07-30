--[[
  DKC2 VRAM Dump Pipeline — Mesen2 Lua Script
  ==============================================
  
  Scans ROM for all unique graphics sets, then allows interactive
  VRAM dumping per gfxset while playing.

  SETUP:
  1. In Mesen: Script > Script Window > Settings (gear icon)
     -> Enable "Allow access to I/O and OS functions"
  2. Load DKC2 ROM (with 102% .srm save for level access)
  3. Open Script > New Script Window, load this script

  USAGE (automatic mode — normal levels):
  - Script runs ROM analysis on startup (shows gfxset count in log)
  - Navigate to a level on the world map and enter it
  - HUD shows: current level, detected gfxset, progress
  - Press [F2] to dump VRAM + CGRAM + PPU state for current gfxset
  - Gfxsets already dumped are marked green in HUD
  - Press [F3] to log remaining gfxsets to the script log

  USAGE (manual slot mode — world maps and NPC shops):
  World maps have no gfxset at all (property type 0x0004/0x0005 carries no
  style data), so lookupGfxsetFromRom returns nil and [F2] refuses to dump
  them. NPC shops do have a gfxset (0x08-0x0C via NPC_GFXSETS) but that only
  helps if WRAM $0539 actually holds it there — unverified. Manual mode sits
  next to the automatic path and takes the target set from YOU instead of
  from RAM, so neither question has to be answered:
  - [Page Up] / [Page Down] select a slot (HUD shows name + target file)
  - [End] dumps the current screen into that slot
  - After a dump the selection advances by one — the slot list is in
    collection order, so walking the maps in order needs no stepping
  - Dumping over an existing file needs a second [End] to confirm

  There is one slot per map/shop, not one per category: slots 1-9 are the
  nine individual maps, slots 10-14 the five shops.

  OUTPUT (per gfxset, in script data folder):
  - gfxset_XX_vram.bin    — 64KB VRAM binary (compatible with Viewer import)
  - gfxset_XX_cgram.bin   — 512B CGRAM palette data
  - gfxset_XX_state.txt   — PPU registers + level metadata
  - gfxset_XX.savestate   — Mesen savestate for future re-dumping

  NOTE: WRAM address $003E is used for level ID detection.
  If detection seems wrong, the HUD will show it — verify visually.
]]--

------------------------------------------------------------------------
-- Configuration
------------------------------------------------------------------------
local DUMP_KEY      = "F2"    -- Key to trigger VRAM dump
local STATUS_KEY    = "F3"    -- Key to log remaining gfxsets
local FRAMES_WAIT   = 3      -- Frames to wait before dump (VBlank DMA settle)

-- Manual slot mode. Key names must match Core/Shared/KeyDefinitions.h
-- ("Page Up" and "Page Down" carry a space). Deliberately not F-keys —
-- those collide with Mesen's own save/load state hotkeys.
local SLOT_PREV_KEY = "Page Up"
local SLOT_NEXT_KEY = "Page Down"
local SLOT_DUMP_KEY = "End"

------------------------------------------------------------------------
-- DKC2 Level Names (indexed 0x00 - 0xBF = 192 entries)
-- Source: p4plus2 disassembly + DKC2 level editor
------------------------------------------------------------------------
local LEVEL_NAMES = {
  [0x00] = "(Unused)",
  [0x01] = "Glimmer's Galleon",
  [0x02] = "Rambi Rumble",
  [0x03] = "Pirate Panic",
  [0x04] = "Gangplank Galley",
  [0x05] = "Rattle Battle",
  [0x06] = "Glimmer's Galleon Exit",
  [0x07] = "Hot-Head Hop",
  [0x08] = "Red-Hot Ride",
  [0x09] = "Krow's Nest",
  [0x0A] = "Slime Climb",
  [0x0B] = "Topsail Trouble",
  [0x0C] = "Mainbrace Mayhem",
  [0x0D] = "Kreepy Krow",
  [0x0E] = "Target Terror",
  [0x0F] = "Rickety Race",
  [0x10] = "Haunted Hall",
  [0x11] = "Hornet Hole",
  [0x12] = "Rambi Rumble - Rambi",
  [0x13] = "Parrot Chute Panic",
  [0x14] = "Lava Lagoon",
  [0x15] = "Lockjaw's Locker",
  [0x16] = "Fiery Furnace",
  [0x17] = "Web Woods",
  [0x18] = "Gusty Glade",
  [0x19] = "Ghostly Grove",
  [0x1A] = "Topsail Trouble Shortcut",
  [0x1B] = "K. Rool Cabin",
  [0x1C] = "Hot-Head Hop Bonus2",
  [0x1D] = "Pirate Panic Shortcut",
  [0x1E] = "Target Terror End",
  [0x1F] = "Web Woods Beta",
  [0x20] = "Mainbrace Mayhem Shortcut",
  [0x21] = "Kleever's Kiln",
  [0x22] = "Rattle Battle - Rattly",
  [0x23] = "Windy Well",
  [0x24] = "Squawks's Shaft",
  [0x25] = "Kannon's Klaim",
  [0x26] = "Parrot Chute Shortcut",
  [0x27] = "Kannon's Klaim Shortcut",
  [0x28] = "Barrel Bayou",
  [0x29] = "Krockhead Klamber",
  [0x2A] = "Web Woods - Squitter",
  [0x2B] = "Barrel Bayou NoWarp",
  [0x2C] = "Mudhole Marsh",
  [0x2D] = "Bramble Blast",
  [0x2E] = "Bramble Scramble",
  [0x2F] = "Screech's Sprint",
  [0x30] = "OW: Gangplank Galleon",
  [0x31] = "OW: Crocodile Cauldron",
  [0x32] = "OW: Krem Quay",
  [0x33] = "OW: Krazy Kremland",
  [0x34] = "OW: Gloomy Gulch",
  [0x35] = "OW: K. Rool's Keep",
  [0x36] = "OW: The Flying Krock",
  [0x37] = "OW: Lost World",
  [0x38] = "OW: Crocodile Isle",
  [0x39] = "Cranky's Cabin",
  [0x3A] = nil,
  [0x3B] = nil,
  [0x3C] = nil,
  [0x3D] = nil,
  [0x3E] = nil,
  [0x3F] = nil,
  [0x40] = "Wrinkly Kollege W1",
  [0x41] = "Wrinkly Kollege W2",
  [0x42] = "Wrinkly Kollege W3",
  [0x43] = "Wrinkly Kollege W4",
  [0x44] = "Wrinkly Kollege W5",
  [0x45] = "Wrinkly Kollege W6",
  [0x46] = "Wrinkly Kollege W7",
  [0x47] = "Wrinkly Kollege W8",
  [0x48] = "Swanky Bonus W1",
  [0x49] = "Swanky Bonus W2",
  [0x4A] = "Swanky Bonus W3",
  [0x4B] = "Swanky Bonus W4",
  [0x4C] = "Swanky Bonus W5",
  [0x4D] = "Swanky Bonus W6",
  [0x4E] = "Swanky Bonus W7",
  [0x4F] = "Swanky Bonus W8",
  [0x50] = "Funky's Flights W1",
  [0x51] = "Funky's Flights W2",
  [0x52] = "Funky's Flights W3",
  [0x53] = "Funky's Flights W4",
  [0x54] = "Funky's Flights W5",
  [0x55] = "Funky's Flights W6",
  [0x56] = "Funky's Flights W7",
  [0x57] = "Funky's Flights W8",
  [0x58] = "Klubba's Kiosk W1",
  [0x59] = "Klubba's Kiosk W2",
  [0x5A] = "Klubba's Kiosk W3",
  [0x5B] = "Klubba's Kiosk W4",
  [0x5C] = "Klubba's Kiosk W5",
  [0x5D] = "Klubba's Kiosk W6",
  [0x5E] = "Klubba's Kiosk W7",
  [0x5F] = "Klubba's Kiosk W8",
  [0x60] = "King Zing Sting",
  [0x61] = "K. Rool Duel",
  [0x62] = "Castle Crush",
  [0x63] = "Kudgel's Kontest",
  [0x64] = nil,
  [0x65] = nil,
  [0x66] = nil,
  [0x67] = nil,
  [0x68] = "Lockjaw's Locker Short",
  [0x69] = "Lava Lagoon Shortcut",
  [0x6A] = "Squawks Shaft Short",
  [0x6B] = "Krocodile Kore",
  [0x6C] = "Arctic Abyss",
  [0x6D] = "Chain Link Chamber",
  [0x6E] = "Toxic Tower",
  [0x6F] = "Pirate Panic Bonus1",
  [0x70] = "Pirate Panic Bonus2",
  [0x71] = "Gangplank Galley Bonus2",
  [0x72] = "Rattle Battle Bonus1",
  [0x73] = "Rattle Battle Bonus3",
  [0x74] = "Hot-Head Hop Bonus3",
  [0x75] = "Hot-Head Hop Bonus1",
  [0x76] = "Red-Hot Ride Bonus1",
  [0x77] = "Red-Hot Ride Bonus2",
  [0x78] = "Mainbrace Mayhem Bonus1",
  [0x79] = "Mainbrace Mayhem Bonus2",
  [0x7A] = "Slime Climb Bonus1",
  [0x7B] = "Topsail Trouble Bonus1",
  [0x7C] = "Topsail Trouble Bonus2",
  [0x7D] = "Mainbrace Mayhem Bonus3",
  [0x7E] = "Slime Climb Bonus2",
  [0x7F] = "Rattle Battle Bonus2",
  [0x80] = "Klobber Karnage",
  [0x81] = "Lockjaw's Locker Bonus1",
  [0x82] = "Glimmer's Galleon Bonus2",
  [0x83] = "Lava Lagoon Bonus1",
  [0x84] = "Glimmer's Galleon Bonus1",
  [0x85] = "Ghostly Grove Bonus1",
  [0x86] = "Gusty Glade Bonus1",
  [0x87] = "Gusty Glade Bonus2",
  [0x88] = "Ghostly Grove Bonus2",
  [0x89] = "Barrel Bayou Bonus1",
  [0x8A] = "Barrel Bayou Bonus2",
  [0x8B] = "Krockhead Klamber Bonus1",
  [0x8C] = "Mudhole Marsh Bonus1",
  [0x8D] = "Mudhole Marsh Bonus2",
  [0x8E] = "Hot-Head/Red-Hot Short",
  [0x8F] = "Clapper's Cavern",
  [0x90] = "Animal Antics Enguarde",
  [0x91] = "Clapper's Cavern Bonus1",
  [0x92] = "Clapper's Cavern Bonus2",
  [0x93] = "Arctic Abyss Bonus1",
  [0x94] = "Black Ice Battle Bonus1",
  [0x95] = "Arctic Abyss Bonus2",
  [0x96] = "Black Ice Battle",
  [0x97] = "Klobber Karnage Bonus1",
  [0x98] = "Jungle Jinx Bonus1",
  [0x99] = "Jungle Jinx",
  [0x9A] = "Animal Antics Rambi",
  [0x9B] = "Animal Antics Squitter",
  [0x9C] = "Animal Antics Rattly",
  [0x9D] = "Animal Antics Bonus1",
  [0x9E] = "Fiery Furnace Bonus1",
  [0x9F] = "Animal Antics Squawks",
  [0xA0] = "Bramble Blast Bonus2",
  [0xA1] = "Target Terror Bonus1",
  [0xA2] = "Bramble Scramble Bonus1",
  [0xA3] = "Windy Well Bonus2",
  [0xA4] = "Web Woods Bonus1",
  [0xA5] = "Toxic Tower Bonus1",
  [0xA6] = "Bramble Blast Bonus1",
  [0xA7] = "Screech's Sprint Bonus1",
  [0xA8] = "Gangplank Galley Bonus1",
  [0xA9] = "Squawks Shaft Bonus3",
  [0xAA] = "Kannon's Klaim Bonus3",
  [0xAB] = "Kannon's Klaim Bonus1",
  [0xAC] = "Squawks Shaft Bonus1",
  [0xAD] = "Kannon's Klaim Bonus2",
  [0xAE] = "Hornet Hole Bonus1",
  [0xAF] = "Parrot Chute Bonus2",
  [0xB0] = "Hornet Hole Bonus3",
  [0xB1] = "Parrot Chute Bonus1",
  [0xB2] = "Rambi Rumble Bonus2",
  [0xB3] = "Hornet Hole Bonus2",
  [0xB4] = "Rambi Rumble Bonus1",
  [0xB5] = "Chain Link Chamber Bonus1",
  [0xB6] = "Chain Link Chamber Bonus2",
  [0xB7] = "Castle Crush Bonus1",
  [0xB8] = "Castle Crush Bonus2",
  [0xB9] = "Stronghold Showdown",
  [0xB9] = "Stronghold Showdown",
  [0xBA] = "Squawks Shaft Bonus2",
  [0xBB] = "Web Woods Bonus2",
  [0xBC] = "Windy Well Bonus1",
  [0xBD] = "Haunted Hall Bonus1",
  [0xBE] = "Rickety Race End",
  [0xBF] = "Haunted Hall End",
}

------------------------------------------------------------------------
-- NPC Shop gfxset overrides (these have property type 0x0004/0x0005
-- and can't be looked up via the normal ROM pointer chain)
------------------------------------------------------------------------
local NPC_GFXSETS = {
  [0x39] = 0x08,  -- Cranky's Cabin
}
-- Wrinkly's Kollege = gfxset 0x0A
for i = 0x40, 0x47 do NPC_GFXSETS[i] = 0x0A end
-- Swanky's Bonus = gfxset 0x0B
for i = 0x48, 0x4F do NPC_GFXSETS[i] = 0x0B end
-- Funky's Flights = gfxset 0x09
for i = 0x50, 0x57 do NPC_GFXSETS[i] = 0x09 end
-- Klubba's Kiosk = gfxset 0x0C
for i = 0x58, 0x5F do NPC_GFXSETS[i] = 0x0C end

------------------------------------------------------------------------
-- Manual slots (world maps + NPC shops)
------------------------------------------------------------------------
-- The 5 shops reuse their real ROM gfxsets 0x08-0x0C, so they drop straight
-- into the existing gfxset_XX naming.
--
-- The 9 world maps have NO gfxset in the ROM and need synthetic indices.
-- A full scan of all 192 level IDs found exactly 32 gfxsets in use:
--   02 03 04 07 08 09 0A 0B 0C 1D 1E 20 21 22 23 24 25 26 27 28 29 2A 2B
--   2C 2D 2E 2F 30 31 32 33 34
-- 0x35-0x3D are free and contiguous — one per map, in world order.
--
-- CAREFUL: the maps' LEVEL IDs are 0x30-0x38, but 0x30-0x34 are real level
-- gfxsets that are already dumped. Naming maps after their level ID would
-- overwrite five existing dumps. The synthetic indices below avoid that.
local MANUAL_SLOTS = {
  { gfx = 0x35, level = 0x38, name = "OW: Crocodile Isle (hub)" },
  { gfx = 0x36, level = 0x30, name = "OW: Gangplank Galleon" },
  { gfx = 0x37, level = 0x31, name = "OW: Crocodile Cauldron" },
  { gfx = 0x38, level = 0x32, name = "OW: Krem Quay" },
  { gfx = 0x39, level = 0x33, name = "OW: Krazy Kremland (screen 1)" },
  { gfx = 0x3A, level = 0x34, name = "OW: Gloomy Gulch" },
  { gfx = 0x3B, level = 0x35, name = "OW: K. Rool's Keep" },
  { gfx = 0x3C, level = 0x36, name = "OW: The Flying Krock" },
  { gfx = 0x3D, level = 0x37, name = "OW: Lost World" },
  { gfx = 0x08, level = 0x39, name = "Shop: Cranky's Monkey Museum" },
  { gfx = 0x09, level = 0x50, name = "Shop: Funky's Flights II" },
  { gfx = 0x0A, level = 0x40, name = "Shop: Wrinkly's Kong Kollege" },
  { gfx = 0x0B, level = 0x48, name = "Shop: Swanky's Bonus Bonanza" },
  { gfx = 0x0C, level = 0x58, name = "Shop: Klubba's Kiosk" },
  -- Krazy Kremland's map spans two screens with different graphics, so it
  -- needs a second dump. Appended rather than inserted next to screen 1 so
  -- the slot numbers of everything already collected stay put.
  { gfx = 0x3E, level = 0x33, name = "OW: Krazy Kremland (screen 2)" },
  -- Non-level screens: no level ID and no gfxset at all, so level is 0.
  -- The boss arenas are deliberately NOT here — they are ordinary levels with
  -- real gfxsets and are already dumped (K. Rool Duel = 0x2E, Krocodile Kore
  -- = 0x2A, King Zing = 0x04, Kudgel = 0x26, Stronghold Showdown = 0x2B).
  { gfx = 0x3F, level = 0x00, name = "Screen: Title" },
  { gfx = 0x40, level = 0x00, name = "Screen: Menu / file select" },
  { gfx = 0x41, level = 0x00, name = "Screen: Game Over" },
  { gfx = 0x42, level = 0x00, name = "Screen: Rating / ending" },
}

------------------------------------------------------------------------
-- ROM data access (DKC2 = HiROM, bank $3D holds level/style data)
------------------------------------------------------------------------
local ROM_BANK_3D = 0x3D0000  -- PRG ROM offset for bank $3D

--- Look up gfxset index from ROM for a given level ID.
--- Returns: gfxset, ppuConfig, mapId  (or nil + error string)
function lookupGfxsetFromRom(levelId)
  -- Check NPC override first
  if NPC_GFXSETS[levelId] then
    return NPC_GFXSETS[levelId], 0, 0
  end

  -- Step 1: Read property pointer from table at $3D:0000
  local ptrAddr = emu.read16(ROM_BANK_3D + levelId * 2, emu.memType.snesPrgRom)

  -- Step 2: Read property type
  local propBase = ROM_BANK_3D + ptrAddr
  local propType = emu.read16(propBase, emu.memType.snesPrgRom)

  -- Overworld / NPC types have no style data
  if propType == 0x0004 or propType == 0x0005 then
    return nil, "overworld"
  end

  -- Step 3: Read style pointer (bonus levels have extra byte before style)
  local styleOffset = propBase + 2  -- skip type word
  if propType == 0x0001 then
    styleOffset = styleOffset + 1   -- skip bonusType byte
  end
  local stylePtr = emu.read16(styleOffset, emu.memType.snesPrgRom)

  -- Step 4: Read fields from style data
  local styleBase = ROM_BANK_3D + stylePtr
  local ppuConfig = emu.read(styleBase + 12, emu.memType.snesPrgRom)
  local graphics = emu.read(styleBase + 13, emu.memType.snesPrgRom)
  local mapId    = emu.read(styleBase + 18, emu.memType.snesPrgRom)

  return graphics, ppuConfig, mapId
end

------------------------------------------------------------------------
-- ROM Scan: find all unique gfxsets and which levels use them
------------------------------------------------------------------------
function scanAllGfxsets()
  local sets = {}         -- gfxsetId -> { levels = {levelId, ...}, representative = levelId }
  local levelToGfx = {}   -- levelId -> gfxsetId
  local totalSets = 0

  for i = 0, 191 do
    local gfx = lookupGfxsetFromRom(i)
    if gfx then
      levelToGfx[i] = gfx
      if not sets[gfx] then
        sets[gfx] = { levels = {}, representative = i }
        totalSets = totalSets + 1
      end
      table.insert(sets[gfx].levels, i)
    end
  end

  return sets, levelToGfx, totalSets
end

------------------------------------------------------------------------
-- Memory dump functions
------------------------------------------------------------------------

--- Dump a memory region to a binary file (byte-by-byte via read16 for speed)
function dumpMemoryToFile(filepath, memType)
  local f = io.open(filepath, "w+b")
  if not f then return false end

  local size = emu.getMemorySize(memType)
  local chunks = {}

  -- Read 2 bytes at a time for ~2x speed
  for i = 0, size - 1, 2 do
    local word = emu.read16(i, memType)
    chunks[#chunks + 1] = string.char(word % 256, math.floor(word / 256))
  end

  f:write(table.concat(chunks))
  f:close()
  return true
end

--- Dump PPU state + metadata to text file
--- label (optional) overrides the LEVEL_NAMES lookup — manual slots name
--- themselves, since a shop's level ID is only a representative of eight.
function dumpStateToFile(filepath, levelId, gfxset, ppuConfig, mapId, label)
  local f = io.open(filepath, "w")
  if not f then return false end

  -- lookupGfxsetFromRom returns (nil, "overworld") for map/shop property
  -- types, so ppuConfig can be a string here — %02X would throw on it.
  if type(ppuConfig) ~= "number" then ppuConfig = 0 end
  if type(mapId)     ~= "number" then mapId = 0 end

  f:write("DKC2 VRAM Dump — GfxSet State\n")
  f:write("================================\n\n")
  f:write(string.format("Timestamp:  %s\n", os.date("%Y-%m-%d %H:%M:%S")))
  f:write(string.format("Level ID:   0x%02X (%d)\n", levelId, levelId))
  f:write(string.format("Level Name: %s\n", label or LEVEL_NAMES[levelId] or "Unknown"))
  f:write(string.format("GfxSet:     0x%02X (%d)\n", gfxset, gfxset))
  f:write(string.format("PPU Config: 0x%02X\n", ppuConfig))
  f:write(string.format("Map ID:     0x%02X\n", mapId))
  f:write("\n")

  -- Dump VRAM size verification
  local vramSize = emu.getMemorySize(emu.memType.snesVideoRam)
  local cgramSize = emu.getMemorySize(emu.memType.snesCgRam)
  f:write(string.format("VRAM size:  %d bytes\n", vramSize))
  f:write(string.format("CGRAM size: %d bytes\n", cgramSize))
  f:write("\n")

  -- Dump all PPU-related state keys
  f:write("PPU Register State:\n")
  f:write("-------------------\n")
  local state = emu.getState()
  local ppuKeys = {}
  for k, _ in pairs(state) do
    if string.find(k, "ppu") or string.find(k, "Ppu")
       or string.find(k, "bg") or string.find(k, "Bg") then
      table.insert(ppuKeys, k)
    end
  end
  table.sort(ppuKeys)
  for _, k in ipairs(ppuKeys) do
    f:write(string.format("  %s = %s\n", k, tostring(state[k])))
  end

  f:close()
  return true
end

------------------------------------------------------------------------
-- State variables
------------------------------------------------------------------------
local allGfxsets = {}       -- gfxsetId -> { levels, representative }
local levelToGfx = {}       -- levelId -> gfxsetId
local totalGfxsetCount = 0  -- total unique gfxsets found in ROM

local dumpedGfxsets = {}    -- gfxsetId -> true (already dumped this session)
local dumpCount = 0         -- number of successful dumps

local dumpPending = false   -- dump request queued
local waitCounter = 0       -- frames waited since request
local savestatePending = nil -- gfxset ID awaiting savestate write

local lastMsg = ""          -- last status message
local lastMsgTimer = 0      -- frames remaining for message display

local keyWasDown = false    -- edge detection for dump key
local statusKeyWasDown = false -- edge detection for status key

-- Manual slot mode
local slotIndex = 1           -- currently selected MANUAL_SLOTS entry
local manualDumped = {}       -- gfxsetId -> true (dumped via manual mode)
local manualCount = 0         -- number of manual slots dumped this session
local overwriteArmed = nil    -- gfxset awaiting a confirming second [End]
local overwriteTimer = 0      -- frames the confirmation stays armed
local manualPending = nil     -- slot queued, waiting out FRAMES_WAIT
local slotPrevWasDown = false
local slotNextWasDown = false
local slotDumpWasDown = false

------------------------------------------------------------------------
-- I/O access check
------------------------------------------------------------------------
local outputFolder = emu.getScriptDataFolder()
if outputFolder == "" then
  emu.displayMessage("DKC2", "ERROR: I/O disabled! Enable in Script Settings.")
  emu.log("=== ERROR ===")
  emu.log("I/O access is disabled. Enable 'Allow access to I/O and OS functions'")
  emu.log("in the Script Window settings (gear icon).")
  return
end

------------------------------------------------------------------------
-- Startup: ROM Analysis
------------------------------------------------------------------------
emu.log("================================================================")
emu.log("  DKC2 VRAM Dump Pipeline — Starting ROM Analysis")
emu.log("================================================================")

allGfxsets, levelToGfx, totalGfxsetCount = scanAllGfxsets()

emu.log(string.format("\nFound %d unique graphics sets across 192 level IDs.\n", totalGfxsetCount))
emu.log("GfxSet -> Levels mapping:")
emu.log("-------------------------")

-- Sort gfxset IDs for display
local sortedGfxIds = {}
for id, _ in pairs(allGfxsets) do
  table.insert(sortedGfxIds, id)
end
table.sort(sortedGfxIds)

for _, gfxId in ipairs(sortedGfxIds) do
  local info = allGfxsets[gfxId]
  local repName = LEVEL_NAMES[info.representative] or "?"
  local levelCount = #info.levels
  -- Show representative level (first one found) as the one to visit
  emu.log(string.format(
    "  GfxSet 0x%02X: %2d levels — visit: 0x%02X (%s)",
    gfxId, levelCount, info.representative, repName
  ))
end

emu.log(string.format(
  "\n>>> You need to visit %d levels (one per gfxset) to capture all VRAM data.",
  totalGfxsetCount
))
emu.log(string.format(">>> Output folder: %s", outputFolder))
emu.log(">>> Press [" .. DUMP_KEY .. "] in-game to dump current gfxset.")
emu.log(">>> Press [" .. STATUS_KEY .. "] to show remaining gfxsets in log.")
emu.log("")
emu.log(string.format(">>> Manual slots (%d): world maps + NPC shops.", #MANUAL_SLOTS))
emu.log(string.format(">>> [%s]/[%s] select, [%s] dump the selected slot.",
  SLOT_PREV_KEY, SLOT_NEXT_KEY, SLOT_DUMP_KEY))
for i, s in ipairs(MANUAL_SLOTS) do
  emu.log(string.format("      %2d. gfxset_%02X  %s", i, s.gfx, s.name))
end
emu.log("================================================================\n")

emu.displayMessage("DKC2", string.format(
  "ROM scan: %d gfxsets found. Press [%s] to dump.", totalGfxsetCount, DUMP_KEY
))

------------------------------------------------------------------------
-- NMI exec callback — needed for emu.createSavestate()
-- Reads NMI vector from ROM interrupt table
------------------------------------------------------------------------
local nmiAddr = emu.read16(0xFFEA, emu.memType.snesPrgRom)
emu.log(string.format("NMI handler at: $%04X (exec callback registered)", nmiAddr))

emu.addMemoryCallback(function(addr, val)
  if savestatePending then
    local gfxId = savestatePending
    savestatePending = nil

    local ok, state = pcall(emu.createSavestate)
    if ok and state then
      local path = string.format("%s\\gfxset_%02X.savestate", outputFolder, gfxId)
      local f = io.open(path, "w+b")
      if f then
        f:write(state)
        f:close()
        emu.log(string.format("  Savestate saved: gfxset_%02X.savestate", gfxId))
      end
    else
      emu.log("  (Savestate capture skipped — not in valid context)")
    end
  end
end, emu.callbackType.exec, nmiAddr, nmiAddr, emu.cpuType.snes)

------------------------------------------------------------------------
-- Read current GfxSet directly from WRAM $0539
-- Verified 2026-06-29: $0539 = 0x07 in Pirate Panic, 0x25 in Mainbrace Mayhem
-- ($003E was wrong — stores a different internal counter)
------------------------------------------------------------------------
function readCurrentGfxset()
  local gfxId = emu.read(0x0539, emu.memType.snesWorkRam)
  if gfxId == 0 or not allGfxsets[gfxId] then
    return nil  -- title screen, menu, or unknown gfxset
  end
  return gfxId
end

------------------------------------------------------------------------
-- Perform the actual dump for a gfxset
------------------------------------------------------------------------
--- slot (optional) = a MANUAL_SLOTS entry. When given, the target set and
--- its metadata come from the slot instead of from the ROM scan — world map
--- slots have no ROM gfxset to look up at all.
function performDump(gfxset, slot)
  local prefix = string.format("%s\\gfxset_%02X", outputFolder, gfxset)

  local repLevelId, ppuConfig, mapId, repName
  if slot then
    repLevelId = slot.level
    ppuConfig, mapId = 0, 0
    repName = slot.name
  else
    -- Get ppuConfig/mapId via representative level from ROM scan
    local info = allGfxsets[gfxset]
    repLevelId = info and info.representative or 0
    local romGfx
    romGfx, ppuConfig, mapId = lookupGfxsetFromRom(repLevelId)
    repName = LEVEL_NAMES[repLevelId] or "?"
  end

  emu.log(string.format("--- Dumping GfxSet 0x%02X (rep: %s) ---",
    gfxset, repName))

  -- Dump VRAM (64KB)
  local vramOk = dumpMemoryToFile(prefix .. "_vram.bin", emu.memType.snesVideoRam)
  if vramOk then
    emu.log("  VRAM:  64KB written -> gfxset_" .. string.format("%02X", gfxset) .. "_vram.bin")
  else
    emu.log("  ERROR: Failed to write VRAM file!")
  end

  -- Dump CGRAM (512B)
  local cgramOk = dumpMemoryToFile(prefix .. "_cgram.bin", emu.memType.snesCgRam)
  if cgramOk then
    emu.log("  CGRAM: 512B written -> gfxset_" .. string.format("%02X", gfxset) .. "_cgram.bin")
  else
    emu.log("  ERROR: Failed to write CGRAM file!")
  end

  -- Dump PPU state
  local stateOk = dumpStateToFile(prefix .. "_state.txt", repLevelId, gfxset,
    ppuConfig, mapId, slot and slot.name or nil)
  if stateOk then
    emu.log("  State: written -> gfxset_" .. string.format("%02X", gfxset) .. "_state.txt")
  end

  -- Queue savestate for next NMI exec callback
  savestatePending = gfxset

  if vramOk and cgramOk then
    dumpedGfxsets[gfxset] = true
    local msg
    if slot then
      manualDumped[gfxset] = true
      manualCount = manualCount + 1
      -- The 5 shop slots ARE part of the ROM scan (gfxset 0x08-0x0C via
      -- NPC_GFXSETS), so they count towards dumpCount as well. The 9 world
      -- map slots are synthetic and would push the HUD past totalGfxsetCount.
      if allGfxsets[gfxset] then dumpCount = dumpCount + 1 end
      msg = string.format("OK: %s -> gfxset_%02X (%d/%d slots)",
        slot.name, gfxset, manualCount, #MANUAL_SLOTS)
      -- Advance to the next slot. The slot list is in collection order, so
      -- the common case (walk to the next map, press [End]) needs no manual
      -- stepping at all — forgetting it is otherwise silent until the
      -- overwrite guard catches it one screen too late.
      if slotIndex < #MANUAL_SLOTS then slotIndex = slotIndex + 1 end
    else
      dumpCount = dumpCount + 1
      msg = string.format("OK: gfxset 0x%02X dumped (%d/%d)",
        gfxset, dumpCount, totalGfxsetCount)
    end
    emu.log("  " .. msg)
    emu.displayMessage("DKC2 Dump", msg)
    lastMsg = msg
  else
    lastMsg = string.format("ERROR dumping gfxset 0x%02X!", gfxset)
    emu.log("  " .. lastMsg)
  end
  lastMsgTimer = 300  -- show for ~5 seconds at 60fps
end

------------------------------------------------------------------------
-- Log remaining (undumped) gfxsets
------------------------------------------------------------------------
function logRemaining()
  local remaining = 0
  emu.log("\n--- Remaining GfxSets ---")
  for _, gfxId in ipairs(sortedGfxIds) do
    if not dumpedGfxsets[gfxId] then
      remaining = remaining + 1
      local info = allGfxsets[gfxId]
      local repName = LEVEL_NAMES[info.representative] or "?"
      emu.log(string.format("  [ ] 0x%02X — visit: %s (0x%02X)",
        gfxId, repName, info.representative))
    end
  end
  emu.log(string.format("--- %d of %d remaining ---\n", remaining, totalGfxsetCount))
  emu.displayMessage("DKC2", string.format("%d/%d gfxsets remaining", remaining, totalGfxsetCount))
end

------------------------------------------------------------------------
-- Main frame callback: HUD + key handling + dump logic
------------------------------------------------------------------------
emu.addEventCallback(function()
  -- Read current gfxset directly from WRAM $0539
  local gfxset = readCurrentGfxset()
  local info = gfxset and allGfxsets[gfxset]
  local repName = info and (LEVEL_NAMES[info.representative] or "?") or "N/A"

  -- Key edge detection (dump key)
  local keyDown = emu.isKeyPressed(DUMP_KEY)
  if keyDown and not keyWasDown then
    if gfxset then
      if dumpedGfxsets[gfxset] then
        lastMsg = string.format("GfxSet 0x%02X already dumped!", gfxset)
        lastMsgTimer = 180
        emu.displayMessage("DKC2", lastMsg)
      else
        dumpPending = true
        waitCounter = 0
      end
    else
      lastMsg = "No gfxset detected (title/menu?)"
      lastMsgTimer = 180
      emu.displayMessage("DKC2", lastMsg)
    end
  end
  keyWasDown = keyDown

  -- Key edge detection (status key)
  local statusDown = emu.isKeyPressed(STATUS_KEY)
  if statusDown and not statusKeyWasDown then
    logRemaining()
  end
  statusKeyWasDown = statusDown

  -- === Manual slot mode: select with Page Up/Down, dump with End ===
  local prevDown = emu.isKeyPressed(SLOT_PREV_KEY)
  if prevDown and not slotPrevWasDown then
    slotIndex = slotIndex - 1
    if slotIndex < 1 then slotIndex = #MANUAL_SLOTS end
    overwriteArmed = nil
  end
  slotPrevWasDown = prevDown

  local nextDown = emu.isKeyPressed(SLOT_NEXT_KEY)
  if nextDown and not slotNextWasDown then
    slotIndex = slotIndex + 1
    if slotIndex > #MANUAL_SLOTS then slotIndex = 1 end
    overwriteArmed = nil
  end
  slotNextWasDown = nextDown

  if overwriteTimer > 0 then
    overwriteTimer = overwriteTimer - 1
    if overwriteTimer == 0 then overwriteArmed = nil end
  end

  local slotDown = emu.isKeyPressed(SLOT_DUMP_KEY)
  if slotDown and not slotDumpWasDown then
    local slot = MANUAL_SLOTS[slotIndex]
    local target = string.format("%s\\gfxset_%02X_vram.bin", outputFolder, slot.gfx)
    local existing = io.open(target, "rb")
    if existing then existing:close() end

    if existing and overwriteArmed ~= slot.gfx then
      -- A mis-selected slot could overwrite a level dump collected weeks ago,
      -- so an existing file always costs a second, deliberate press.
      overwriteArmed = slot.gfx
      overwriteTimer = 300
      lastMsg = string.format("gfxset_%02X exists — [%s] again to overwrite",
        slot.gfx, SLOT_DUMP_KEY)
      lastMsgTimer = 300
      emu.displayMessage("DKC2 Dump", lastMsg)
    else
      overwriteArmed = nil
      emu.log(string.format("[manual] slot '%s' -> gfxset_%02X", slot.name, slot.gfx))
      manualPending = slot   -- same VBlank-DMA settle delay as the [F2] path
      waitCounter = 0
    end
  end
  slotDumpWasDown = slotDown

  -- Dump delay countdown
  if dumpPending then
    waitCounter = waitCounter + 1
    if waitCounter >= FRAMES_WAIT then
      dumpPending = false
      -- Re-read in case level changed during wait
      gfxset = readCurrentGfxset()
      if gfxset and not dumpedGfxsets[gfxset] then
        performDump(gfxset)
      end
    end
  elseif manualPending then
    waitCounter = waitCounter + 1
    if waitCounter >= FRAMES_WAIT then
      local slot = manualPending
      manualPending = nil
      -- No re-read here: the slot is the user's explicit choice, not RAM state.
      performDump(slot.gfx, slot)
    end
  end

  -- === Draw HUD ===
  local x = 8
  local y = 8
  local white   = 0xFFFFFF
  local yellow  = 0xFFFF00
  local green   = 0x00FF00
  local orange  = 0xFF8800
  local red     = 0xFF4444
  local gray    = 0xA0A0A0
  local bgColor = 0xC0000000  -- semi-transparent black

  -- Title
  emu.drawString(x, y, "DKC2 VRAM Dump", white, bgColor, 0, 1)
  y = y + 11

  -- Current gfxset + representative level name
  if gfxset then
    local gfxColor = dumpedGfxsets[gfxset] and green or orange
    local marker = dumpedGfxsets[gfxset] and " [DONE]" or ""
    local gfxStr = string.format("Gfx: 0x%02X — %s%s", gfxset, repName, marker)
    emu.drawString(x, y, gfxStr, gfxColor, bgColor, 0, 1)
  else
    emu.drawString(x, y, "Gfx: N/A (title/menu/unknown)", gray, bgColor, 0, 1)
  end
  y = y + 11

  -- Progress
  local progStr = string.format("Done: %d / %d gfxsets", dumpCount, totalGfxsetCount)
  emu.drawString(x, y, progStr, green, bgColor, 0, 1)
  y = y + 11

  -- Manual slot selection (world maps + shops)
  local slot = MANUAL_SLOTS[slotIndex]
  local slotColor = manualDumped[slot.gfx] and green or yellow
  local slotMark  = manualDumped[slot.gfx] and " [DONE]" or ""
  emu.drawString(x, y, string.format("Slot %d/%d: %s", slotIndex, #MANUAL_SLOTS, slot.name),
    slotColor, bgColor, 0, 1)
  y = y + 11
  emu.drawString(x, y, string.format("  -> gfxset_%02X%s  (manual %d/%d)",
    slot.gfx, slotMark, manualCount, #MANUAL_SLOTS), slotColor, bgColor, 0, 1)
  y = y + 11

  -- Dump in progress indicator
  if dumpPending or manualPending then
    local remaining = FRAMES_WAIT - waitCounter
    emu.drawString(x, y, "DUMPING in " .. remaining .. "...", red, bgColor, 0, 1)
    y = y + 11
  else
    emu.drawString(x, y, "[" .. DUMP_KEY .. "]=Dump [" .. STATUS_KEY .. "]=Status", gray, bgColor, 0, 1)
    y = y + 11
    emu.drawString(x, y, "[PgUp/PgDn]=Slot [" .. SLOT_DUMP_KEY .. "]=Dump slot", gray, bgColor, 0, 1)
    y = y + 11
  end

  -- Last message
  if lastMsgTimer > 0 then
    lastMsgTimer = lastMsgTimer - 1
    local msgColor = string.find(lastMsg, "ERROR") and red or green
    emu.drawString(x, y, lastMsg, msgColor, bgColor, 0, 1)
  end

end, emu.eventType.endFrame)

------------------------------------------------------------------------
-- Done
------------------------------------------------------------------------
emu.log("Script ready. Waiting for input...")
