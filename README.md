# FlatOut 2 VR

6DoF VR, SimHub telemetry, wheelbase force feedback and wheel + pedal input for **FlatOut 2**
(PC, v1.2), ported from the NFSU2 VR mod. Rendering goes through [DXVK](https://github.com/doitsujin/dxvk)
(Direct3D 9 on Vulkan), which also lets the mod hand frames to SteamVR on the GPU.

| Folder | Output | What it does |
|---|---|---|
| `FO2-VR` | `scripts\FO2HeadTrack.asi` | Headset drives the camera from the driver's seat, depth-based stereo 3D, SteamVR scene app, in-car gauges |
| `FO2-SimHub` | `scripts\FO2Telemetry.asi` | SimHub "External sims" telemetry, DirectInput force feedback, wheel + separate pedals fed into the game |
| `FO2-Settings` | `FlatOut 2 Mod Settings.exe` | Edits both ini files with their comments as descriptions |

## Features

**VR (`FO2HeadTrack`)**
- Headset rotation and position move the in-game camera (6DoF), seated in the driver's seat of the car body.
  Works in every in-game camera view; the bumper view keeps the car body out of the way.
- Stereo: `Mode=1` builds both eyes from every frame using the game's depth buffer (full frame rate per eye);
  `Mode=0` renders left and right eye on alternating frames.
- Runs as a SteamVR scene app: each picture is submitted with the head pose it was rendered with, so
  SteamVR keeps it world-locked between game frames. Overlay mode is still available (`VRFrames=0`).
- With DXVK, frames go to SteamVR on the GPU (shared D3D11 textures imported into DXVK's Vulkan device).
  Without DXVK it falls back to desktop capture.
- Round rev counter and speedometer plus a position / lap / lap time bar, fixed on the dashboard in 3D.
  Faces and needles are PNGs in `gauge_mod`.
- Fixed field of view while driving (`GameFOV`, `NoSpeedZoom`): the game's speed zoom is switched off.
- Keys: F7 tracking on/off, F8 recenter, F9 save, F10 screen on/off, Ctrl+arrows/PgUp/PgDn move the seat,
  Ctrl+Shift+arrows/PgUp/PgDn size and place the screen.

**SimHub, force feedback, wheel and pedals (`FO2Telemetry`)**
- FlatOut 2 appears as a game in SimHub (no plugin needed): speed, rpm, gear, position, laps, lap times,
  orientation and rates, accelerations, velocities, world position, throttle/brake/handbrake, slip,
  suspension velocity, nitro. Works for dashboards, rev lights, ShakeIt and motion.
- Force feedback through DirectInput (built for the Fanatec CSL DD): self-aligning torque from the front
  axle's slip (countersteer in slides, light wheel past the grip peak), speed-dependent weight, damper,
  bumps and impact kicks.
- Wheel steering and separate pedals (e.g. SIMSONN Plus X) are written into the player's controls right
  before the game reads them, because FlatOut 2 can only use one controller.

## Requirements

- FlatOut 2 **v1.2** (`FlatOut2.exe`, 5,369,344 bytes). Both ASIs check the game code and stay inactive on other versions.
- An ASI loader. The nya mod loader (`winmm.dll` + `xnya_modloader.toml`) already loads `scripts/*.asi`.
- SteamVR (e.g. with Virtual Desktop).
- Recommended: [DXVK](https://github.com/doitsujin/dxvk/releases), the 32-bit `x32\d3d9.dll`.
- For telemetry: [SimHub](https://www.simhubdash.com) with External sims support.

## Install

1. Build (below) or download the `FlatOut2-VR` artifact from the latest GitHub Actions run.
2. Copy into the FlatOut 2 folder:
   - `FO2-VR/install/scripts/*` and the built `FO2HeadTrack.asi` into `scripts\`
   - `FO2-VR/install/gauge_mod` into `gauge_mod\`
   - `FO2-SimHub/FO2Telemetry.ini`, `FO2Telemetry.simdef` and the built `FO2Telemetry.asi` into `scripts\`
   - optional: `FlatOut 2 Mod Settings.exe` into the game folder
3. DXVK: copy `x32\d3d9.dll` from the DXVK release and `FO2-VR/optional/dxvk.conf` into the game folder.
4. In the game: turn off motion blur, and turn off force feedback if FO2Telemetry drives the wheel.
5. Start SteamVR, then the game. In a race, sit straight, look ahead and press **F8**.
   Adjust the seat with Ctrl + arrows / PageUp / PageDown and save with F9.

User guide: `FO2-VR/install/VR-HeadTracking.txt`. All settings are commented in the ini files.

### SimHub telemetry signature

SimHub checks two signatures in every packet. The game signature is derived from the definition's id
and is computed by the ASI. The layout signature is computed by SimHub when it loads the definition and
could not be reproduced here, so the shipped `FO2Telemetry.simdef` has `"TelemetrySignature": 0`.
If SimHub lists FlatOut 2 but shows no data:

1. Open SimHub's External sims editor for "FlatOut 2" and copy the telemetry signature it shows.
2. Put it in `scripts\FO2Telemetry.ini` as `TelemetrySignature=` (hex), or save the definition from the
   editor so the `.simdef` carries it. The ASI reads it at game start and logs the values it sends.

## Building

- Visual Studio 2022 Build Tools with the C++ workload: run `FO2-VR/src/build.bat` and
  `FO2-SimHub/src/build.bat` (32-bit), and `FO2-Settings/build.bat` for the settings app.
- GitHub Actions (`.github/workflows/build.yml`) builds all three with MSVC on every push and uploads a
  ready-to-copy `FlatOut2-VR` folder.
- `third_party/` contains the OpenVR header (Valve, BSD-3-Clause) and the Vulkan headers (Khronos, Apache-2.0 / MIT).

## How it hooks the game

- Camera: the call that renders all views (`0x479311` -> `0x4C9DC0`) is redirected. During the render, the
  player camera's matrix (`camera + 0x80`, rows right/up/forward/position) is replaced by car body
  (`Car + 0x1B0`) + seat offset + head pose and restored afterwards, so the renderer's view, culling and
  LOD follow your head while the game's own camera logic is untouched.
- Field of view: the same camera's frustum (`camera + 0xF0`, which the render function passes to
  `DeviceD3d::SetFrustum`) is scaled to `GameFOV` for the render and restored afterwards.
- Input: `Player::ApplyControls` (`0x46FA50`, called at `0x46FA34`), with steering, throttle, brake and
  handbrake at `Player + 0x684 .. 0x690`.
- Data: engine rpm `Car + 0x5D8`, gear `Car + 0x634`, velocity `Car + 0x280`, pedals `Car + 0x1DF4 ..`,
  race data from `pGameFlow` (`0x8E8410`), the player host and `pScoreManager` (`0x8DA5D0`).

## Status

The code builds, but it has not been run in the game yet. Everything below the game hooks (OpenVR,
depth stereo, DXVK hand-over, overlays, gauges, force feedback model) is the tested NFSU2 code. The
FlatOut 2 addresses come from reverse engineering `FlatOut2.exe` v1.2; the least certain ones are:
- engine rpm (`Car + 0x5D8`) and suspension travel (`Car + 0x1890 + 0x40 * i + 0x18`),
- steering direction (`SteerInvert` in `FO2Telemetry.ini` flips it),
- race position and lap numbering (0- or 1-based is detected at runtime).

The logs (`scripts\FO2HeadTrack.log`, `scripts\FO2Telemetry.log`) say which hooks were installed.
If the game crashes or stops drawing, `FO2HeadTrack.log` records where (code address, registers, stack).
`[DEBUG]` in `FO2HeadTrack.ini` and `[INPUT] Enabled` in `FO2Telemetry.ini` switch single hooks off.
The UG2 version's minimap-on-the-dashboard instrument is not ported.

## Credits

DXVK by Philip Rebohle and contributors, OpenVR by Valve. FlatOut 2 structure layouts and global
addresses partly from gaycoderprincess's [nya-common-fo2](https://github.com/gaycoderprincess/nya-common-fo2) (MPL-2.0).
