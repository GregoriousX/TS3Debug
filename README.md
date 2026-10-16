# TS3Debug

An ASI plugin for **Toy Story 3: Complete Edition** (PC, Steam) that turns on debug
features left in the game: a **free camera**, the hidden **cheat list**
(god mode, infinite ammo, time control, wireframe…) and **keyboard flight** for the free camera.

## Features

| Key | Action |
|-----|--------|
| **F1** | Free camera on / off |
| **F2** | Free camera on → freeze it in place and give control back to the player → off |
| **F3** | God mode |
| **F4** | Stealth |
| **F5** | Infinite ammo |
| **F6** | Unlock all levels |
| **F7** | Stop time |
| **F8** | Wireframe rendering |
| **F9** | Game speed: normal → 2x → 1/2x → 1/4x → 1/8x → normal |
| **F11** | "Say Stress" (audio debug: voice line stress test) |

**Free camera controls**

| Input | Keyboard | Gamepad |
|-------|----------|---------|
| Move | W A S D | Left stick |
| Look | Arrow keys | Right stick |
| Up / down | Space / C | RB / RT |
| Faster / slower | Left Shift / Left Ctrl | — |

Tip: **Stop time (F7) + free camera (F1)** works as a photo mode.

The hotkeys only work while you are in a level, not in the menus. When you toggle a cheat,
the game plays its own "cheat on/off" sound.

## Installation

1. Download `TS3Debug.zip` from the [Releases](../../releases) page.
2. Extract it into the game folder, next to `Toy Story 3 Complete Edition.exe`:
   ```
   Toy Story 3 Complete Edition\
   ├── Toy Story 3 Complete Edition.exe
   ├── dinput8.dll        ← Ultimate ASI Loader
   ├── TS3Debug.asi
   └── TS3Debug.ini
   ```
   If you already have an ASI loader installed, copy only `TS3Debug.asi` and `TS3Debug.ini`.
3. Start the game. On a successful start `TS3Debug.log` is created next to the exe.

To uninstall, delete `dinput8.dll`, `TS3Debug.asi`, `TS3Debug.ini` and `TS3Debug.log`.

## Configuration

Every key and option can be changed in `TS3Debug.ini`. You can write a key as
`F1`–`F12`, as a single letter (`W`), or as a
[virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)
(`0x20` = Space).

| Option | Default | Description |
|--------|---------|-------------|
| `VoiceFeedback` | `1` | Play the game's cheat on/off sound |
| `KeyboardControl` | `1` | Fly the free camera with the keyboard |
| `FastMultiplier` / `SlowMultiplier` | `3.0` / `0.25` | Speed modifiers (Shift / Ctrl) |
| `LookSpeed` | `1.0` | Arrow-key look speed |
| `InvertForward` / `InvertPitch` | `1` / `0` | Flip these if W flies backwards or Up looks down |

## Troubleshooting

- **No `TS3Debug.log` appears.** The ASI loader isn't running. Check that `dinput8.dll` is in
  the same folder as the exe.
- **The log says `unsupported game version`.** A game update changed the code. The plugin
  disables itself instead of patching the wrong place. Please open an issue and attach the log.
- **Nothing happens when I press a key.** Make sure you are in a level, not in a menu,
  and that the game window has focus.

## How it works

- All game addresses are found at startup by signature scan. Nothing is hardcoded. If a
  game update breaks a signature, the plugin switches itself off and writes the reason to the log
  instead of patching the wrong place.
- The free camera is the engine's own. The plugin only switches it on and off.
- Cheats come from the game's own cheat table. They are looked up by name and toggled the same way the game does it.
- The plugin hooks a function the game calls once per frame. Hotkeys are handled there, so all game
  code runs on the game's main thread.
- To add keyboard flight, the plugin patches only the free camera's input handler. Normal gameplay controls
  are left alone.

## Building

You need Visual Studio 2022 Build Tools (or newer) with the C++ x64 toolset.

```bat
build.bat
```

The output goes to `bin\TS3Debug.asi`.

## Credits

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by ThirteenAG
- Toy Story 3 © Disney/Pixar. This is an unofficial fan project and is not affiliated with Disney, Pixar or Avalanche Software.
