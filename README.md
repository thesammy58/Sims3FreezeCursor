# Sims3FreezeCursor

A tiny, standalone native mod for The Sims 3: when the simulation thread
stalls for more than a second, it swaps the mouse cursor to the game's own
busy-cursor spinner (the same one shown during a normal load screen), so a
frozen game reads visually the same way a legitimate load already does,
instead of leaving you staring at an unchanged arrow cursor with no signal
anything is wrong.

That's the entire feature. No on-screen indicator, no dialog, no settings
panel - just the cursor swap. It started as one piece of a larger
freeze-rescue feature (an on-screen indicator plus a 20-second "still
waiting? Save / Reset" dialog) built on top of
[Sims3MonoModder (S3MM)](https://github.com/sims3fiend/Sims3MonoModder),
and was split out for anyone who wants just the cursor cue without a whole
modding toolkit.

**Works on Steam, retail and EA App installs**, and runs alongside other
mods without special handling - see [Playing well with other
mods](#playing-well-with-other-mods) below.

## Installation

1. Install an ASI loader into `The Sims 3\Game\Bin`, if you don't already
   have one (S3MM and S3SS need the same thing, so if either is installed
   you already have this):
   - [dxwrapper](https://github.com/elishacloud/dxwrapper), or
   - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest)

2. Drop `Sims3FreezeCursor.asi` into `The Sims 3\Game\Bin`.

3. Launch the game. That's it - no config file, nothing to set up.

A log is written to `Sims3FreezeCursor.log` **next to the `.asi`**, in
`Game\Bin`. If that folder isn't writable (a locked-down install under
Program Files, say), it goes to
`%LOCALAPPDATA%\Sims3FreezeCursor\` instead - the first line of the log
always names the version, so you can tell which file you're looking at.

## Playing well with other mods

This mod doesn't modify the function it watches. It rewrites the single
`call` instruction that reaches it, and leaves the function's own bytes
exactly as the game shipped them.

That matters because several Sims 3 mods find the same engine functions by
scanning for their opening bytes. A mod that overwrites those bytes breaks
everyone else's search - and that's a real bug this project shipped once
and has since fixed properly rather than worked around. Install order
doesn't matter, and nothing waits on anything else.

## Reporting a problem

Please include `Sims3FreezeCursor.log` - the first few lines identify the
mod version, your game build, and how the hook attached, which is most of
the diagnosis.

If you're asked to narrow something down, an optional
`Sims3FreezeCursor.ini` next to the `.asi` can switch individual pieces
off. **You don't need this for normal use** - with no file present
everything runs on the defaults below.

```ini
[Sims3FreezeCursor]

; 0 = don't install the stall detector at all (the mod loads but does nothing)
; 1 = normal
SimHook=1

; 0 = don't subclass the game window; the cursor never changes
CursorHook=1

; Seconds between throughput lines in the log. 0 is off.
; Set to 30 if you're reporting stutter - the log otherwise only records
; changes, so a game running badly can produce an empty one.
HeartbeatStatsSeconds=0

; Writes a detailed register/stack dump on an access violation anywhere in
; the process. A debugging aid, not a feature - only turn it on if asked.
CrashCapture=0
```

## Building from source

Requires **Visual Studio 2022** (the *Desktop development with C++*
workload) and **[vcpkg](https://github.com/microsoft/vcpkg)**.

1. Run `setup-deps.bat` once. It finds `vcpkg.exe` via `VCPKG_ROOT` or a few
   common install locations, and installs the one dependency this project
   has - [Detours](https://github.com/microsoft/Detours), used only by the
   opt-in `HookMethod=2` prologue detour - into `vcpkg_installed\`.
2. Open `Sims3FreezeCursor.sln` and build **Release | x86** (or Debug).
   The game is 32-bit, so there is no x64 configuration.
3. The built `.asi` lands in `bin\<Configuration>\Sims3FreezeCursor.asi`.

### Checking a game build offline

`tools/s3fc_callsites.py` (Python 3, no packages needed) answers the
question "will the hook install on this build?" without launching the
game. It counts how many places match the `ProcessTasks` byte pattern and
how many `call`s reach the match - one of each means it will.

```
python tools\s3fc_callsites.py "C:\path\to\TS3W.exe"
```

It needs an unpacked executable. Steam and retail `TS3W.exe` are fine; the
EA App's `TS3.exe` is encrypted on disk and reports zero of everything,
which is the encryption, not a result.

## How it works, briefly

- A watchdog thread polls a counter every 150ms. The counter is bumped by a
  hook on `MonoScriptHost::ProcessTasks`, the engine's own simulation task
  pump - if a second passes without it moving, the sim thread is considered
  stalled. The hook itself does one increment and reads no clock; the
  watchdog thread owns the timing.
- The hook is installed by rewriting the one `call` instruction that
  reaches that function, located by scanning for calls whose target matches
  the resolved address. If a game build doesn't have exactly one such call,
  the mod says so in the log and installs nothing rather than guessing.
- While stalled, a subclassed WndProc on the game's own window intercepts
  `WM_SETCURSOR` and forces the game's built-in busy-cursor resource (ID
  103) instead of letting the game repaint its normal cursor. Windows only
  delivers that message when the pointer moves, so the watchdog posts one
  itself on each state change - otherwise a stall that began while you sat
  still would show nothing until you happened to move the mouse.
- The game's window is found via `EnumWindows`, not through any D3D9 hook -
  this mod never touches D3D9 at all.

See [`src/`](src/) - it's a small enough codebase to read end to end in one
sitting.

## Credits

Thanks to **Just Harry** for suggesting the call-site hook and for pointing
out how much a synchronous log write can cost, both of which made this
noticeably better; and to **leonid** for the patience to test a great many
builds on the one machine where the original bug reproduced.

## License

[GNU General Public License v3.0](LICENSE). Parts of `src/` are adapted
from [Sims3MonoModder](https://github.com/sims3fiend/Sims3MonoModder),
which is also GPL-3.0; the comments in each file say what came from where.
