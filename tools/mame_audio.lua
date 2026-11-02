-- Reference audio from MAME: mame bloodbro ... -wavwrite out.wav -autoboot_script tools/mame_audio.lua
local frame = 0
local ioport = manager.machine.ioport
local function setf(port, name, v) local p = ioport.ports[port]; if not p then return end local f = p.fields[name]; if f then f:set_value(v) end end
emu.register_frame_done(function()
  frame = frame + 1
  local nf = frame
  setf(":COIN","Coin 1", (nf>=700 and nf<706) and 1 or 0)
  setf(":IN1","1 Player Start", (nf>=760 and nf<766) and 1 or 0)
  if frame >= 1500 then manager.machine:exit() end
end, "aud")
