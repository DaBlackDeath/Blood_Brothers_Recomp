-- Reference screenshots from MAME for pixel comparison with this project.
-- usage: mame bloodbro -rompath <dir> -video none -sound none -nothrottle -skip_gameinfo \
--        -snapshot_directory /tmp/snap -autoboot_script tools/mame_shot.lua -autoboot_delay 0
-- Inputs match: --press coin1@700+6 --press start1@760+6 --press right@1200+300 --press b1@1300+200
local frame = 0
local shots = {[60]=true,[200]=true,[400]=true,[740]=true,[780]=true,[1500]=true,[2200]=true}
local ioport = manager.machine.ioport
local function setf(port, name, v)
  local p = ioport.ports[port]; if not p then return end
  local f = p.fields[name]; if f then f:set_value(v) end
end
emu.register_frame_done(function()
  frame = frame + 1
  local f = frame - 1   -- number of the frame that just finished (0-based)
  if shots[f] then
    manager.machine.video:snapshot()
    local n = io.open("/tmp/snap/index.txt","a"); n:write(f.."\n"); n:close()
  end
  local nf = f + 1
  setf(":COIN","Coin 1", (nf>=700 and nf<706) and 1 or 0)
  setf(":IN1","1 Player Start", (nf>=760 and nf<766) and 1 or 0)
  setf(":IN0","P1 Right", (nf>=1200 and nf<1500) and 1 or 0)
  setf(":IN0","P1 Button 1", (nf>=1300 and nf<1500) and 1 or 0)
  if f >= 2201 then manager.machine:exit() end
end, "shots")
