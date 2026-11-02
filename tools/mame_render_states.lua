-- Renders saved game states with MAME so that the picture can be compared with bloodbro's own renderer.
--   BB_STATE_DIR=<dir with list.txt, dumps and .crtc files> mame bloodbro -video none -sound none -nothrottle \
--       -snapshot_directory <out> -autoboot_script tools/mame_render_states.lua
-- For each state it writes work RAM, the scroll registers and the layer mask into MAME, keeps MAME's own game logic
-- from running (the wait-for-VBLANK loop at 0x8F0 is replaced by "bra.s *") and takes a snapshot.
local dir = os.getenv("BB_STATE_DIR")
local list = {}
for l in io.lines(dir .. "/list.txt") do list[#list + 1] = l end
local cpu = manager.machine.devices[":maincpu"]
local sp = cpu.spaces["program"]
local rom = manager.machine.memory.regions[":maincpu"]
local idx, phase, wait = 1, 0, -60

local function rd(name)
  local f = io.open(dir .. "/" .. name, "rb"); local s = f:read("a"); f:close(); return s
end
local function load(name)
  local s = rd(name)
  for i = 0, 0xffff, 2 do
    local a, b = s:byte(i + 1, i + 2)
    sp:write_u16(0x80000 + i, a * 256 + b)
  end
  local v = {}
  for h in rd(name .. ".crtc"):gmatch("%x+") do v[#v + 1] = tonumber(h, 16) end
  for k = 0, 5 do sp:write_u16(0xc0020 + 2 * k, v[k + 1]) end
  sp:write_u16(0xc001c, v[7])
end

emu.register_frame_done(function()
  if idx > #list then manager.machine:exit() return end
  if wait < 0 then
    wait = wait + 1
    if wait == 0 then rom:write_u16(0x8f0, 0x60fe) end
    return
  end
  if phase == 0 then load(list[idx]); phase = 1; wait = 0
  else
    wait = wait + 1
    if wait >= 3 then
      manager.machine.video:snapshot()
      idx = idx + 1; phase = 0
    end
  end
end, "render_states")
