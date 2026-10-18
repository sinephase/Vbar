# Vbar

Vertical minimalist task bar for Windows 11 (also runs on Windows 10). Every running window and pinned app is a compact, old-school bevel button with its name and a tiny 2-bit monochrome version of its icon, packed down one edge of the screen.

- **Running windows** at the top, **pinned apps** (your own + your Windows Start menu pins) below, then a **clock** and a `░▒▓█` **start button** at the very bottom
- **Word-wrapped** titles, so long names stay readable instead of truncating; text sits beside the icon and wraps underneath it once it clears, however big the icon is
- **2-bit icons** — each app's real icon, contrast-stretched and reduced to transparent + 3 grey shades (optionally dithered)
- **Scroll indicator** — a thin blue thumb on the inner edge of any section that overflows
- **Focused window** drawn pressed with a white border; minimized windows dimmed; buttons brighten on hover
- **Attention flashing** — a window asking for attention (like a new message) blinks blue, then stays lit until you focus it
- **Hover to focus** — resting on a button brings that window to the front instantly
- **Per-app audio meter** — a thin blue bar under any app that's playing sound, tracking its output level
- **Right-click menus** — restore / minimize / maximize / close / end process / open file location / run as admin
- Reserves its screen edge like the real taskbar, so maximized windows stop beside it
- Any screen, left or right edge
- Everything configurable in a plain-text `vbar.ini`
- Single ~3 MB static exe, no dependencies, no installer

## Usage

1. Download [`bin/vbar.exe`](bin/vbar.exe) (click it, then **Download raw file**) and put it in its own folder.
2. Run it. It writes `vbar.ini` next to itself on first launch.
3. To start it with Windows, put a shortcut to `vbar.exe` in `shell:startup`.

To replace the Windows taskbar entirely: turn on taskbar auto-hide in Windows settings, then set `hidetaskbar=1`. The real taskbar comes back when vbar exits.

| Mouse | Running window | Pinned app | Clock / empty space |
|---|---|---|---|
| Hover | Bring to front | — | — |
| Left-click | Focus, or minimize if already focused | Launch | — (start button: Start menu) |
| Middle-click | Close | — | — |
| Right-click | Window menu | Pin menu | Position, reload, edit config, exit (start button: Win+X menu) |
| Wheel | Scroll the window list | Scroll the pinned list | — |

## Configuration

Edit `vbar.ini` (right-click the clock → **Edit config**), then right-click → **Reload**. Sizes are physical pixels, colours are `#RRGGBB`, and comments go on their own line.

**Layout:** `width`, `spacing` (between buttons), `gap` (between sections), `pad` / `vpad` (text padding), `linegap`, `maxlines`, `ellipsis`, `highlight` (corner highlight length, 0 = full edges)

**Position:** `monitor` (`primary` or a display number from Settings → Display → Identify), `side` (`left` / `right`). Also settable from the right-click **Position** menu.

**Font:** `font`, `fontsize`, `bold`, `fontweight` (100–900), `italic`, `aa` (0 = crisp, 1 = ClearType), `fontfile` (a .ttf/.fon next to the exe, loaded privately — e.g. Terminus)

**Colours:** `bg`, `outline`, `hi`, `face`, `activeface`, `pinface`, `clockface`, `text`, `hovertext`, `activetext`, `dim`, `activeborder`, `hoverbright` (face brightening on hover, 0 = off)

**Attention flash:** `flash` (colour, defaults to `metercolor`), `flashrate` (ms per blink), `flashcount` (blinks before staying lit, 0 = solid)

**Behaviour:** `bordermode` (`focused` / `visible` / `off`), `hoverfocus`, `hoverdelay` (ms, 0 = instant), `hoverrestore`, `hidetaskbar`

**Clock & start button:** `clock`, `clockfontsize` (0 = same as `fontsize`), `clocklines` (wraps if too wide), `clockbold`, `startbutton`, `startheight` (0 = one text line), `startcolor`

**Icons:** `icons`, `iconsize` (0 = text line height), `iconpad`, `icondither`, `iconshades` (three colours, darkest to lightest)

**Scroll indicator:** `scrollw` (px, 0 = off), `scrollcolor` (defaults to `metercolor`)

**Audio meter:** `meter`, `meterh`, `metercolor`, `meterdb` (dB range across full width, 0 = linear), `meterfall`, `meterfps`

**Pins:** `startpins` (import Windows Start pins), `pinsmax` (max % of bar height), `pin=Name|path|args|workingdir` (env vars allowed), `pindir` (a folder of shortcuts), `strip=` (title suffixes to trim, e.g. `- Google Chrome`)

## Building

Single file, Win32 + GDI + WASAPI, no other dependencies.

MSVC (x64 Native Tools prompt):
```
cl /O2 /EHsc /DUNICODE /D_UNICODE vbar.cpp /link /SUBSYSTEM:WINDOWS
```

MinGW-w64:
```
g++ -O2 -municode -mwindows -static vbar.cpp -o vbar.exe -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -luuid -lmsimg32
```

## License

GPL-3.0 — see [LICENSE](LICENSE).
