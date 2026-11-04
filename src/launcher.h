// Launcher window (Windows only): opens when bloodbro.exe is started without arguments, edits bloodbro.ini
// and starts the game as "bloodbro.exe --play".
#pragma once

#ifdef _WIN32
int run_launcher();
// true when bloodbro.ini says "skip the launcher" ([launcher] skip=1) and Shift is not held
bool launcher_skipped();
#endif
