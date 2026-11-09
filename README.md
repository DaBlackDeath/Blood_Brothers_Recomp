# Blood Bros. (TAD Corporation, 1990) – statisch rekompilierter Windows-Port

Windows-Port des Arcade-Spiels **Blood Bros.** (MAME-Set `bloodbro`). Das 68000-Programm des Spiels ist statisch
nach C++ übersetzt und läuft ohne Interpreter; Z80, YM3812, OKI, Grafik und Ein-/Ausgabe bildet die Hardware-Laufzeit
nach. Ergebnis ist **eine einzige `bloodbro.exe`** (Launcher und Spiel, statisch gelinkt, keine DLL nötig).

Das Projekt enthält **keine ROMs**. Du brauchst dein eigenes, legal erworbenes `bloodbro.zip`. Unterstützt wird nur
das Hauptset `bloodbro`: Der rekompilierte Code ist eine Übersetzung genau dieses Programms (ein Merged-ZIP mit
Clones funktioniert, die Clones werden nicht benutzt).

## Bauen mit MSYS2

MSYS2 installieren (<https://www.msys2.org>), dann die **„MSYS2 MINGW64"**-Shell öffnen:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja mingw-w64-x86_64-sdl3 mingw-w64-x86_64-python \
                   mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-glslang

cd /pfad/zum/projekt          # roms/bloodbro.zip muss dort liegen
bash go.sh
```

`go.sh` erzeugt zuerst aus deinem ROM den rekompilierten Code (`tools/recomp_pipeline.sh` → `generated/`), baut dann
`build/bloodbro.exe` und entfernt die Symbole. Das Übersetzen des generierten Codes dauert einige Minuten; spätere
Läufe kompilieren ihn nur neu, wenn er sich geändert hat.

Die exe ist eigenständig: `bloodbro.exe` und `roms\bloodbro.zip` in einen Ordner legen, fertig. Sie braucht nur
Windows-Systembibliotheken (u. a. `opengl32.dll`); `vulkan-1.dll` bringt der Grafiktreiber mit und wird erst zur
Laufzeit geladen (fehlt sie, läuft das Spiel mit OpenGL).

Vulkan-Header und `glslang` (übersetzt die Shader in `src/shaders/` beim Bauen nach SPIR-V) sind optional: Fehlen
sie, baut CMake ohne Vulkan-Renderer (Warnung beim Konfigurieren); abschalten geht auch mit `-DBLOODBRO_VULKAN=OFF`.

## Start und Launcher

- **Doppelklick auf `bloodbro.exe`** öffnet den Launcher (Dear ImGui, mit Maus, Tastatur oder Gamepad bedienbar).
  Er bearbeitet dieselbe `bloodbro.ini`, die auch das Spiel liest und schreibt, und hat fünf Seiten:
  **Game** (ROM-Datei mit Dateiauswahl und CRC-Prüfung, Ton), **Video** (Fenstergröße, Seitenverhältnis, Vollbild,
  Integer-Skalierung, VSync, Renderer, Preset, Pixelfilter, Bezel und alle CRT-Regler), **Controls** (Tasten, Gamepad-Belegung,
  angeschlossene Pads), **Game Rules** (alle DIP-Schalter) und **Cheats** (dieselbe Liste wie im F2-Menü).
  **PLAY** speichert und startet das Spiel (`bloodbro.exe --play`). Der Launcher wartet im Hintergrund und liest die
  INI nach dem Spielende neu ein, damit Änderungen aus den F1/F2/F4-Menüs sichtbar werden. Optional schließt er sich
  nach dem Start (`[launcher] close_after_start`) oder wird ganz übersprungen (`[launcher] skip`, dann startet ein
  Doppelklick sofort das Spiel; mit gedrückter **Shift**-Taste erscheint der Launcher wieder).
- **Mit Argumenten** (z. B. ZIP auf die exe ziehen, `bloodbro.exe roms\bloodbro.zip`, `--play`, `--fullscreen` …)
  startet direkt das Spiel. `bloodbro.exe --help` zeigt alle Optionen.

## Menüs, Einstellungen und Cheats

| Taste | Funktion |
| --- | --- |
| **F1** | DIP-Schalter (SW1/SW2): Münzmodus, Münzen, Leben, Bonus, Schwierigkeit, Continue, Demo-Ton |
| **F2** | Cheats |
| **F4** | Video: CRT-Shader, Presets, Filter, Seitenverhältnis, Integer-Skalierung |
| F3 | Spiel neu starten (nötig, damit manche DIP-Schalter wirken) |
| F10 / F11 / Esc | Pixelfilter durchschalten / Vollbild / Beenden (Esc schließt zuerst ein offenes Menü) |

Im Menü: Pfeiltasten (oder Gamepad-Steuerkreuz), **Enter** (A) schaltet um bzw. führt aus, Links/Rechts ändert
den Wert, Esc (B) schließt. Das Spiel ist angehalten, solange ein Menü offen ist.

- **`bloodbro.ini`** liegt neben `bloodbro.exe` und wird beim Start gelesen und bei jeder Änderung im Menü und
  beim Beenden geschrieben: Fenstergröße, Vollbild, Video, Ton, ROM-Pfad, DIP-Schalter und der Zustand jedes Cheats.
  Kommandozeilen-Optionen haben Vorrang (`--ini DATEI`, `--no-ini`).
- **Cheats:** Die Cheat-Liste ist fest in `bloodbro.exe` eingebaut (16 Einträge, `src/cheats_builtin.inc`, aus
  `data/bloodbro_cheats.xml`; ändern: XML bearbeiten, `python3 tools/embed_cheats.py data/bloodbro_cheats.xml
  src/cheats_builtin.inc`, neu bauen). Eine `bloodbro.xml` neben dem Programm (auch `cheat\bloodbro.xml` oder
  `--cheat-file`) hat Vorrang. Unterstützt: Parameter-Listen, `on`/`off`/`run`/`change`-Skripte, Bedingungen,
  Variablen und die MAME-Ausdrücke (`BAND`, `~`, `maincpu.pb@…`, `mw` …). Cheats, die **Programmcode** patchen,
  wirken im rekompilierten Programm nicht (Hinweis auf der Konsole); Daten-Patches im ROM (z. B. Paletten) und alle
  RAM-Cheats funktionieren. „Invincibility“ nutzt deshalb den Schalter `0x080008` statt eines Code-Patches.
  Fehlerhafte Einträge erscheinen mit `[!]`. Zum Testen: `--cheat "Invincibility"`, `--cheat "Select Starting Stage=6"`.
  Die Liste stammt aus dem MAME-Cheat-Projekt; wer das Programm weitergibt, sollte die Herkunft angeben.
- **Kein Konsolenfenster:** `bloodbro.exe` ist ein Fensterprogramm. Wenn es nicht startet (kein ROM, falsches ROM,
  Grafikfehler), erscheint eine Fehlermeldung mit der Ursache. Aus einer Konsole gestartet schreibt es dorthin.
  Wer immer eine Konsole will: `cmake -S . -B build -DBLOODBRO_CONSOLE=ON`.
- Das ROM wird gefunden über die INI (`[game] rom=`), in `roms\bloodbro.zip` oder `bloodbro.zip` neben dem Programm.

## CRT-Shader (F4)

Das Bild wird mit **Vulkan** oder **OpenGL 2.1** gezeichnet; beide nutzen denselben CRT-Shader
(`src/shaders/crt.frag` für Vulkan, GLSL 1.20 in `src/gl_renderer.cpp` für OpenGL – bei Änderungen beide anpassen).
Welcher Renderer läuft, steht unter `[video] renderer=` in der INI, im Launcher (Video → Renderer) oder auf der
Kommandozeile (`--renderer auto|vulkan|opengl|sdl`, gilt nur für diesen Start):

| Wert | Verhalten |
| --- | --- |
| `auto` (Standard) | Vulkan, sonst OpenGL, sonst SDL-Renderer |
| `vulkan` / `opengl` | der gewählte, sonst der andere, sonst SDL-Renderer |
| `sdl` | einfacher SDL-Renderer ohne Effekte (wie `--no-gl`) |

Beim Start steht auf der Konsole, welcher Renderer und welche GPU benutzt werden. `BLOODBRO_VK_VALIDATION=1`
schaltet die Vulkan-Validierung ein (wenn das Vulkan SDK installiert ist). Der Shader rechnet in linearem Licht
und bietet:

| Regler | Wirkung |
| --- | --- |
| Scanlines | dunkle Zeilen zwischen den Bildzeilen, helle Pixel haben einen „dickeren Strahl“ |
| Shadow mask | RGB-Streifen (Aperture Grille), Helligkeit wird ausgeglichen |
| Screen curvature | gewölbter Bildschirm mit abgerundeten schwarzen Ecken |
| Glow | etwas Licht aus den Nachbarpixeln |
| Vignette | Abdunklung zum Rand |
| Phosphor afterglow | Nachleuchten: helle Pixel verblassen über einige Frames (bei 100 % etwa eine halbe Sekunde) |
| Brightness, Saturation | Gesamthelligkeit, Farbsättigung |
| Pixel filter | *Sharp bilinear*, *Linear*, *Nearest* |
| Aspect ratio | 4:3 (Arcade-Monitor), quadratische Pixel, oder ans Fenster angepasst |
| Integer scaling | nur ganzzahlige Vergrößerung; mit Scanlines und Maske am saubersten |
| Bezel | Monitorrahmen ein/aus (siehe unten) |

Presets: *Custom*, *Off (clean pixels)*, *Scanlines*, *CRT*, *CRT strong*, *CRT BlackDeath*. Wer einen Effekt-Regler
(Scanlines … Saturation, Filter, Bezel) verstellt, landet bei *Custom*. Die eigenen Custom-Werte bleiben erhalten:
Wählt man ein Preset und danach wieder *Custom* (im F4-Menü oder im Launcher), kommen sie genau so zurück, auch nach
einem Neustart (`custom_*` in der INI). Seitenverhältnis und Integer-Skalierung gehören zu keinem Preset.
Alles wird sofort angewendet und unter `[video]` in `bloodbro.ini` gespeichert. Auf der Kommandozeile:
`--shader crt`, `--integer-scale`, `--renderer NAME`. Tipp: Für Scanlines die Fenstergröße so wählen, dass die Höhe
ein Vielfaches von 224 ist (448, 672, 896 …), oder *Integer scaling* einschalten.

Stehen weder Vulkan noch OpenGL zur Verfügung (alter Treiber, Remote-Desktop …), läuft das Spiel mit dem normalen
SDL-Renderer weiter (ohne Effekte). Im Menü steht dann ein Hinweis, auf der Konsole der Grund.

## Bezel (Monitorrahmen)

Liegt ein Rahmenbild vor, wird es benutzt: **`bezel\bloodbro.png`** (auch `bezels\bloodbro.png`, `bloodbro_bezel.png`
oder `bezel.png`) neben `bloodbro.exe`, oder `--bezel DATEI`, oder `bezel_file=` unter `[video]` in der INI.
Das Bild ist eine PNG mit einem **transparenten Bildschirmfenster**; das Programm findet das Fenster selbst und
zeichnet das Spiel hinein, den Rahmen darüber. Hat die PNG kein transparentes Fenster, dient sie als Hintergrund.
F4 → *Bezel* schaltet den Rahmen ein/aus, `--no-bezel` ignoriert Rahmenbilder. Ein Beispiel liegt unter
`data/bezel_example.png`: Nach `bezel\bloodbro.png` kopieren, um es zu sehen.

## Steuerung

| Aktion | Taste |
| --- | --- |
| Bewegen | Pfeiltasten |
| Feuer / Rolle / Dynamit | Z / X / C |
| Münze / Start (Spieler 1) | 5 / 1 |
| Münze / Start (Spieler 2) | 6 / 2 |
| Spieler 2 | I J K L, Buttons U O P |
| Service | 9 |
| Reset / Filter / Vollbild / Ende | F3 / F10 / F11 / Esc |

Gamepads werden für Spieler 1 und 2 erkannt (Hotplug möglich). Bekannte Pads (SDL-Mapping): A/B/X =
Feuer/Rolle/Dynamit, Start = Start, Back = Münze. Unbekannte Pads laufen über einen Fallback (Achse 0/1 oder Hat,
Buttons 0/1/2 = Feuer/Rolle/Dynamit, 6 oder 8 = Münze, 7 oder 9 = Start). Mit `--pad-debug` werden alle
Buttons/Achsen ausgegeben.

## Rekompilierung

Der Code in `generated/` ist eine Übersetzung des Spielprogramms und liegt deshalb **nicht** im Projekt; er wird
lokal aus deinem ROM erzeugt:

```bash
bash tools/recomp_pipeline.sh roms/bloodbro.zip   # dump_prg.py + gen_recomp.py (nur python3)
```

- `tools/dump_prg.py` holt das 512-KB-Programm aus der ZIP, `tools/gen_recomp.py` übersetzt jede der **20.809**
  Instruktionen (ausgeführt + statisch erreichbar) in eine C++-Funktion und fasst sie zu 8.850 Basic-Blöcken zusammen.
  Eingaben: `data/analysis_bloodbro.json` (Funktionen, Sprungziele), `data/m68k_cycles.bin` (Zyklentabelle),
  `data/symbols_bloodbro.txt` (Namen als Kommentare im erzeugten Code).
- `src/recomp_cpu.cpp` springt Block für Block durch den erzeugten Code und prüft Interrupts. Es gibt keinen
  Interpreter mehr: Beim Start wird jeder übersetzte Opcode mit dem geladenen ROM verglichen (falsches Set oder
  verändertes Programm → Fehlermeldung), und Code außerhalb der Übersetzung beendet das Spiel mit einer Meldung samt
  Adresse (kommt im Spiel nicht vor).
- Verifiziert wurde der Generator, solange das Projekt noch den Musashi-Interpreter enthielt: 7,6 Mio. Zufallstests
  über alle Instruktionen, ein Sweep über den ganzen 68000-Befehlssatz und 4 × 60.000 Frames Lockstep (Register, RAM,
  Zyklen, Bild, Ton nach jeder Scanline) ohne eine Abweichung.
- Die Analyse-Daten stammen aus einem Ausführungs-Trace (`data/trace_bloodbro.txt`) dieser früheren Version;
  `tools/analyze_trace.py` (braucht `pip install capstone`) erzeugt daraus `data/analysis_bloodbro.json`.
- Der Trace reicht nur bis Stage 4-2. Stages und Objekte springt das Spiel über Tabellen an
  (`lea TABELLE,a3` / `movea.l (a3,d0.w),a3` / `jsr (a3)`); was der Trace nie sah, fehlte deshalb in der Übersetzung
  (ab Stage 4-3: „code … that is not part of the recompiled program“). `tools/extend_analysis.py prg.bin
  data/analysis_bloodbro.json` findet diese 11 Tabellen statisch, nimmt alle Einträge auf und disassembliert von dort
  weiter (+3.797 Instruktionen, u. a. die Stages 4-3 bis 5-4 und 11 Objekttypen). Nach einem neuen Lauf von
  `analyze_trace.py` muss es erneut laufen. Getestet mit `--cheat "Select Starting Stage=N" --cheat Invincibility
  --autoplay SEED --stage-log PREFIX` für alle Stages.

## Das Spiel verstehen und verändern

- `data/symbols_bloodbro.txt`: benannte Hardware-Register, RAM-Variablen und Funktionen.
- `data/function_index_bloodbro.txt` (`tools/function_index.py`): alle 534 Funktionen mit Größe, Aufrufern,
  Aufgerufenen und den benannten Variablen, die sie anfassen.
- `docs/GAME_NOTES.md`: Speicherkarte, Spielvariablen, Programmstruktur und die Ergebnisse der Analyse.

## Stand

Geprüft gegen MAME 0.264 (gleiche Eingaben, gleiche Frames):

- **Bild:** Logo-Fade, Highscore-Liste, Titelbild, Credit-Anzeige und Stage-Intro sind pixelgleich zu MAME.
  `bash tools/verify_renderer.sh` (braucht MAME) vergleicht 75 Spielzustände aus vier Stages: 74 pixelidentisch.
- **Ton:** YM3812 + OKI + Seibu-Interrupts, Lautstärkeverlauf ≈ 1,00 zu MAME.
- 68000 und Z80 werden pro Scanline verschränkt, nicht zyklengenau wie in MAME; Animationen laufen im Spiel
  manchmal ±1 Frame versetzt. Der „Extra-Layer" (CRTC Bit 2) und Bildschirm-Flip sind nicht umgesetzt (im MAME-Treiber
  ebenfalls nicht).

## Aufbau

```text
src/main.cpp          SDL3-Frontend, Renderer-Auswahl, Eingabe, Audioausgabe, Testoptionen
src/launcher.*        Launcher-Fenster (Dear ImGui, SDL3 + OpenGL 2), src/bloodbro.rc + .manifest
src/machine.*         Speicherkarte 68000/Z80, Seibu-Sound-Interface, CRTC, Scheduling
src/recomp_cpu.cpp    Dispatcher des rekompilierten 68000-Programms, src/cpu_backend.h Schnittstelle
recomp/               Laufzeit des rekompilierten Codes (CPU-Zustand, Operationen)
generated/            erzeugter Code (aus deinem ROM, nicht weitergeben)
src/video.*           Tilemaps + Sprites (Software-Renderer)
src/renderer.*        gemeinsame Schnittstelle der GPU-Renderer, Nachleuchten (CPU)
src/vk_renderer.*     Vulkan-Renderer, src/shaders/ seine GLSL-Shader (beim Bauen → SPIR-V)
src/gl_renderer.*     OpenGL-Renderer mit CRT-Shader, src/video_settings.h Presets und Optionen
src/oki6295.*         OKI-ADPCM
src/romset.*          ROM-Laden (ZIP/Ordner) mit CRC-Prüfung
src/cheats.*          Cheat-Engine für das MAME-Cheat-Format, src/cheats_builtin.inc eingebaute Liste
src/menu.*            F1/F2/F4-Menüs (eigene 8x8-Schrift), src/ini.* Einstellungen
src/bezel.*           Rahmenbild (PNG über stb_image) mit automatischer Fenster-Erkennung
tests/                Tests für Menü, Presets, Cheat-Ausdrücke, INI und Bezel
tools/                Pipeline, Generator, Analyse, Cheat-Einbettung, MAME-Vergleichsskripte
data/                 Analyse- und Trace-Daten (nur Adressen), Zyklentabelle, Cheats, Symbole
third_party/          superzazu/z80 (kleiner Patch), ymfm (YM3812), miniz, stb_image, Dear ImGui
```

## Tests und Debug-Hilfen

- `menu_test`, `bezel_test`: Menülogik (inklusive Stresstest über alle Seiten, Zeilen und Tasten),
  Presets und Custom-Werte, Cheat-Ausdrücke, INI, Bezel-Erkennung. Sie werden nur auf Wunsch gebaut:
  `cmake -S . -B build_tests -G Ninja -DBLOODBRO_TESTS=ON && cmake --build build_tests`, danach
  `build_tests/menu_test` und `build_tests/bezel_test`. Mit Speicherprüfung zusätzlich `-DBLOODBRO_SANITIZE=ON`.
- `--headless --frames N --shot F:datei.png --press coin1@700+6 --wav out.wav --autoplay SEED`: reproduzierbare Läufe.
- `BB_LAYER_MASK=0x1a` (Layer abschalten), `BB_DUMP_RAM=prefix` (RAM + CRTC am Ende sichern), `--ram-dump`,
  `--stage-log`, `--watch-write`, `--poke`.
- `tools/`: MAME-Lua-Skripte, `compare_frames.py`, `verify_renderer.sh`, `ui_smoke_test.sh` (Linux, xvfb).

## Lizenzen

Eigener Code: noch nicht festgelegt. Fremdcode in `third_party/` behält seine Lizenz:
superzazu/z80 (MIT), ymfm (BSD-3, A. Giles), miniz (MIT), stb_image (Public Domain/MIT), Dear ImGui 1.93 (MIT, O. Cornut).
SDL3 (zlib-Lizenz) ist statisch eingebunden. Hardware-Wissen (Speicherkarte, Grafik-Layouts, Seibu-Interface)
stammt aus dem MAME-Treiber; die Zyklentabelle `data/m68k_cycles.bin` aus Musashi (K. Stenerud).
