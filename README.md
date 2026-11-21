# NEON RUN — 2D Car Racing

A compact, native Windows arcade racing game written in C++17. It draws its cars, road, menus, and HUD with GDI, synthesizes a looping chiptune and event-specific sound effects with Windows audio APIs, and needs no third-party libraries or assets.

## Build

Install CMake and a C++17 Windows compiler (Visual Studio Build Tools or MinGW), then run from this folder:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run `build\Release\NeonRun.exe` (Visual Studio) or `build\NeonRun.exe` (MinGW).

## Controls and screens

- **Arrow keys** or **A / D**: steer
- **P**: pause or resume during a run
- **Enter**: start, retry, or confirm a screen
- **H**: instructions; **L**: leaderboard; **M**: sound toggle
- **N**: edit your player name from the menu; Enter saves it
- **Esc**: return to the menu (or close it from the main menu)
- Click menu buttons as an alternative to keyboard shortcuts

Traffic gets faster as a run continues. This is a one-life game: contact between either car's body or tyres ends the run. The top eight scores are stored in `highscores.txt` beside the game executable, so names and records persist between sessions.

The sound system plays separate non-blocking cues for steering, passing traffic, menu actions, starting, pausing, resuming, crashes, and new records. **M** toggles both music and effects.

Traffic uses shared flow speed within each lane, and a lane only receives another car after there is enough room. This prevents cars from catching and stacking on top of one another.

The road has four marked lanes. Frames are rendered into a back buffer to prevent flicker. Steering input is cleared on focus loss, and one-shot keyboard commands ignore key repeat.

## Project files

- `main.cpp` — game loop, rendering, input, collision, sound, and score storage
- `CMakeLists.txt` — minimal Windows build configuration
- [ER diagram](outputs/neon-run-er-diagram.jpg) — Chen-style logical model for documentation (the game currently persists leaderboard rows in `highscores.txt`)
- [Project presentation](outputs/Neon-Run-Project-Presentation.pptx) — eight-slide overview with a proposed five-week Gantt chart
