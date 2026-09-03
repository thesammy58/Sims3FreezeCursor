# Sims3FreezeCursor

A tiny, standalone native mod for The Sims 3: when the simulation thread
stalls for more than a second, it swaps the mouse cursor to TS3.exe's own
busy-cursor spinner (the same one shown during a normal load screen), so a
frozen game reads visually the same way a legitimate load already does,
instead of leaving you staring at an unchanged arrow cursor with no signal
anything is wrong.

That's the entire feature. No on-screen indicator, no dialog, no settings
panel - just the cursor swap. It was split out of [Sims3MonoModder
(S3MM)](../Sims3MonoModder-1.2.0-beta), which has the full version of this
feature (on-screen indicator + a 20-second "still waiting? Save / Reset"
dialog) alongside its much larger modding toolkit. This project is for
anyone who wants just the cursor cue without any of that.

**Runs fine with or without S3MM installed at the same time.** Both mods
hook the same native function and subclass the same game window, and both
are built so a second hook/subclass installs cleanly on top of the first
regardless of which one loads first - see the comments in `addresses.h`,
`core/sim_heartbeat_hook.cpp`, and `core/cursor_hook.h` for the specifics.

## Installation

1. Install an ASI loader into `The Sims 3\Game\Bin`, if you don't already
   have one (S3MM needs the same thing, so if it's installed you already
   have this):
   - [dxwrapper](https://github.com/elishacloud/dxwrapper), or
   - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest)

2. Drop `Sims3FreezeCursor.asi` into `The Sims 3\Game\Bin`.

3. Launch the game. That's it - no config file, nothing to set up. A log
   file is written to
   `Documents\Electronic Arts\The Sims 3\Sims3FreezeCursor\Sims3FreezeCursor.log`
   if you ever want to check it's picking up heartbeats.

## Building from source

Requires **Visual Studio 2022** and **vcpkg**.

1. Run `setup-deps.bat` once (it finds `vcpkg.exe` via `VCPKG_ROOT` or a few
   common install locations, and installs the one dependency this project
   has - [Detours](https://github.com/microsoft/Detours), for the native
   hook).
2. Open `Sims3FreezeCursor.vcxproj` in Visual Studio and build
   (Debug or Release, Win32).
3. The built `.asi` lands in `bin\<Configuration>\Sims3FreezeCursor.asi`.

## How it works, briefly

- A watchdog thread polls a heartbeat timestamp every 150ms. The heartbeat
  is stamped by a Detours hook on `MonoScriptHost::ProcessTasks` (the
  engine's own per-frame simulation task pump) - if 1 second passes with no
  new stamp, the sim thread is considered stalled.
- While stalled, a subclassed WndProc on the game's own window intercepts
  `WM_SETCURSOR` and forces TS3.exe's built-in busy-cursor resource (ID 103)
  instead of letting the game repaint its normal cursor.
- The game's own window is found via `EnumWindows`, not through any D3D9
  hook - this mod never touches D3D9 at all.

See [`src/`](src/) - it's a small enough codebase to read end to end in one
sitting.
