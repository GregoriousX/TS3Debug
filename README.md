# TS3Debug

An ASI plugin for **Toy Story 3: Complete Edition** (PC, Steam) that turns on debug
features left in the game: a **free camera**, **god mode**, hidden cheats (stealth, infinite ammo),
**pause / slow motion / frame-by-frame advance**, **save / load position** and **teleporting**,
an **actor spawner** with 43 characters and creatures, and a **debug menu** to drive it all with the mouse.

## Features

| Key | Action |
|-----|--------|
| **Home** | Show / hide the debug menu |
| **F1** | Free camera on / off |
| **F2** | Free camera on → freeze it in place and give control back to the player → off |
| **F3** | God mode (health stays full and hits can't kill you; you still flinch when hit) |
| **F4** | Stealth (enemies don't notice your shots or impacts) |
| **F5** | Infinite ammo for the player's weapons (guns, throwables and Buzz's laser) |
| **F6** | Pause / resume time |
| **F7** | Game speed: normal → 2x → 1/2x → 1/4x → 1/8x → normal |
| **F8** | Frame advance: pauses time if needed, then each press steps one frame (F6 resumes) |
| **Insert** | Spawn the selected actor in front of the player |
| **Page Down / Page Up** | Select the next / previous actor |
| **Ctrl + Numpad 1/2/3** | Save the character's position to slot 1/2/3 (the modifier is configurable) |
| **Numpad 1/2/3** | Teleport the character to saved position 1/2/3 |
| **End** | Teleport the character to the free camera and leave free camera mode |
| **Delete** | Fly with your character: it follows the free camera until you turn this off |

**Debug menu (Home):** a small always-on-top window with buttons for every feature, the
time controls and a list of all spawnable actors (double-click to spawn). It never takes focus away
from the game, so you can keep playing while it's open. Play in windowed or borderless mode to
see it on top of the game.

**Free camera controls**

| Input | Keyboard | Gamepad |
|-------|----------|---------|
| Move | W A S D | Left stick |
| Look | Arrow keys | Right stick |
| Up / down | Space / C | RB / RT |
| Faster / slower | Left Shift / Left Ctrl | — |

**For speedrunners:** save a position before a tricky section and reload it as often as you like.
Saved positions are stored in `TS3Debug.ini`, so they survive restarts. They are just coordinates,
so load a slot only in the level where you saved it. To get somewhere new, turn on the free camera (F1),
fly there and press End. Or press Delete to fly with your character and switch it off where you want to land.

Tip: **Pause (F6) + free camera (F1)** works as a photo mode, and **F8** lets you step through
the action one frame at a time.

**Spawnable actors:** Alien, Barnacle, Bat, Bendito, Blockhead, Bullseye, Caterpillar, CoinShip,
Cow, Dragon, Fairy, Gargoyle, Ghost, LittleGreenMan, Mule, Monkey, Muffin, MuffinBoss,
DollyWitchBoss, Slimes, ZurgBot, ZurgBotCaptain, ZurgMinion, ZurgSpider, UFOs, Dr. Porkchop's UFO,
Hamm's mothership, Orphan, TNT barrel, pets (chicken, goat, pigs) and more. Most of them belong in the
Toy Box. In story levels, actors whose assets aren't loaded may not appear.

Game actions only run while you are in a level. Anything you click in the menu outside a level
waits until you are back in one. When you toggle a cheat,
the game plays its own "cheat on/off" sound.

## Installation

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
   (x64) if you don't have an ASI loader yet: put its `dinput8.dll` in the game folder, next to
   `Toy Story 3 Complete Edition.exe`. The loader is not included in `TS3Debug.zip`.
2. Download `TS3Debug.zip` from the [Releases](../../releases) page and extract it into the same folder:
   ```
   Toy Story 3 Complete Edition\
   ├── Toy Story 3 Complete Edition.exe
   ├── dinput8.dll        ← Ultimate ASI Loader (step 1)
   ├── TS3Debug.asi
   └── TS3Debug.ini
   ```
3. Start the game. On a successful start `TS3Debug.log` is created next to the exe.

To uninstall, delete `dinput8.dll`, `TS3Debug.asi`, `TS3Debug.ini` and `TS3Debug.log`.

## Configuration

**Rebinding keys:** open the debug menu (Home) and click **Key bindings...**. Pick an action, click
**Change key** (or double-click it) and press the new key. Esc cancels. The change applies at once
and is saved to `TS3Debug.ini`. Menu buttons always show the current keys.

Every key and option can also be changed in `TS3Debug.ini`. You can write a key as
`F1`–`F12`, as a single letter (`W`), or as a
[virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes)
(`0x20` = Space).

| Option | Default | Description |
|--------|---------|-------------|
| `VoiceFeedback` | `1` | Play the game's cheat on/off sound |
| `ShowMenuOnStart` | `0` | Open the debug menu when the game starts |
| `FlyHeightBelowCamera` | `1.5` | How far below the free camera your character is carried while flying |
| `GodBlocksInstantKill` | `0` | God mode also blocks instant kills, such as falling off the level (you may fall forever) |
| `FrameStepFps` | `30` | How much time one frame advance step covers |
| `SpawnDistance` | `5.0` | How far in front of the player actors spawn |
| `KeyboardControl` | `1` | Fly the free camera with the keyboard |
| `FastMultiplier` / `SlowMultiplier` | `3.0` / `0.25` | Speed modifiers (Shift / Ctrl) |
| `LookSpeed` | `1.0` | Arrow-key look speed |
| `InvertForward` / `InvertPitch` | `1` / `0` | Flip these if W flies backwards or Up looks down |

## Troubleshooting

- **No `TS3Debug.log` appears.** The ASI loader isn't running. Check that `dinput8.dll` is in
  the same folder as the exe.
- **The log says `unsupported game version`.** A game update changed the code. The plugin
  disables itself instead of patching the wrong place. Please open an issue and attach the log.
- **What about "Unlock levels", "Wireframe" and "Say Stress"?** The game's internal cheat table lists
  them (and its own "God"), but the code that would use them was removed from the retail game.
  God mode in this plugin is implemented separately.
- **I can't see the debug menu.** It can't draw over exclusive fullscreen. Switch the game to
  windowed or borderless mode.
- **Nothing happens when I press a key.** Make sure you are in a level, not in a menu,
  and that the game window has focus.

## How it works

- All game addresses are found at startup by signature scan. Nothing is hardcoded. If a
  game update breaks a signature, the plugin switches itself off and writes the reason to the log
  instead of patching the wrong place.
- The free camera is the engine's own. The plugin switches it on and off and works around two engine
  bugs: from the second level load on, the game created a duplicate free camera that froze both the
  player and the camera, and cutscenes leave camera state behind that made the free camera show the
  last cutscene frame or crash the game. The free camera can't be switched on during a cutscene.
- Pause, game speed and frame advance drive the game's master timer directly, using its built-in
  speed settings and single-step mode.
- Position save/load and teleporting move the character the same way the game's own level scripts do.
- God mode lets hits play out normally, so knock-backs and recovery work as usual. It caps
  damage so a hit can never be lethal, then refills the player's health.
- The actor spawner builds each actor from its parameter list and passes it to the game's own actor creator, the same way the game spawns actors in levels.
- Stealth is a flag in the game's own cheat table, which the game still reads. The game only
  applies it to a "cheat avatar" that the retail game never sets, so the plugin sets it to the player.
- Infinite ammo tops up the player's weapons every frame. The game's own ammo cheat skips Buzz's
  laser and some throwables, so the plugin doesn't use it.
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
