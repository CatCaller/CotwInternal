# CotwInternal

Internal x64 helper for **theHunter: Call of the Wild** (`theHunterCotW_F.exe`, Steam).

Use it only in your own private/single-player sessions. The game has a co-op mode; do not use it in sessions with other players.

## Features

- **Always-on orange animal outlines** - a D3D11/D3D12 auto-detecting overlay always draws through-wall orange outlines around every animal in the local animal manager. There is no fill and no per-distance colour.
- **No smell (F2)** - zeros the scent block of every animal perception configuration.
- **Stealth / no senses (F2)** - also zeros the vision and hearing blocks of the same configuration, so animals no longer see, hear or smell the player.
- **2x speed (F3)** - scales the local character physics step input horizontal velocity (`sub_14025A310`) and restores it immediately after the step.

## Configuration

The DLL creates `CotwInternal.ini` next to itself. Defaults:

```ini
[General]
EnableNoSenses=1
EnableNoSmell=1
EnableSpeed=1
SpeedMultiplier=2.0
HighlightDistance=600

[Keys]
ToggleSenses=113
ToggleSpeed=114
```

A log is written to `CotwInternal.log` next to the DLL:

## Build

Open `CotwInternal.sln` in Visual Studio and build `Release|x64`, or:

```powershell
msbuild CotwInternal.sln /p:Configuration=Release /p:Platform=x64
```

Output: `build\Release\CotwInternal.dll`.

The project is deliberately linked without the C runtime:

- `/NODEFAULTLIB`
- custom `memcpy` / `memset` / `memmove`
- `/GS-`, `/Gs9999999`, `/GR-`, exceptions disabled
- imports only `KERNEL32.dll`, `USER32.dll`, `d3d11.dll` and `dxgi.dll` (`d3d12.dll` resolved dynamically if present)


## Injection

Use a normal x64 `LoadLibrary` injector or a manual map injector.

## Offsets and verification

Mapped against the installed `theHunterCotW_F.exe` build code **3366163**; the IDA database was verified on 2026-11-17.

- Player vtable (`0x1EE24A0`), local player chain (`0x27F1478`), character physics step (`0x25A310` and its 6 call/jmp sites), animal world/manager vector (`0x27C5DC8` -> `+0xB8` -> `+0x28..+0x30`), animal character/position/species layout, camera/view-projection globals (`0x27E72F4` / `0x27E7404`), AI sense system/manager (`0x27C5D50` / vtable `0x1A4F6C0`) and the sense-config copier call site (`0x1405F931D` -> `0x1406014B0`) were verified directly in the IDA database.
