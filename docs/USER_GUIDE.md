# User guide

## Main screens

The normal flow is:

```text
Clock -> Launcher -> ROM browser -> MSX
             |             |
             +-> Settings  +-> MSX1 / MSX2 / MSX2+
             +-> Wi-Fi Upload
             +-> Display tests
```

- Press the physical **BOOT** button to open the launcher or return from a
  browser/game screen.
- Press **PWR** to turn the AMOLED off or wake it. Emulation remains managed by
  the firmware; PWR is not the same as quitting a game.
- Tap launcher and browser rows directly on the screen.

## Starting software

1. Open `ROM BROWSER`.
2. Set `MODEL` to MSX1, MSX2, or MSX2+ as required by the title.
3. Tap a `.rom`, `.dsk`, or `.cas` file.

The default is MSX2. C-BIOS is suitable mainly for cartridge ROM software.
Disk and cassette software usually needs a compatible BIOS environment that
is not included.

## Controller mapping

The validated controller is an 8BitDo Micro in K mode. It appears as `80EL`.

| Control | In a game | In menus/keyboard |
|---|---|---|
| D-pad | MSX joystick directions | Move selection/cursor |
| A | Fire A | Confirm or type selected key |
| B | Fire B | Back, close or resume |
| START | Toggle software keyboard | — |
| SELECT | Toggle MSX menu | — |
| L2 | Level-control modifier | Toggle SHIFT page while keyboard is open |

A short directional press moves one item. Holding a direction repeats after
a short delay.

## Volume and brightness

Outside the software keyboard, hold **L2** and press:

- Up: volume +5%
- Down: volume -5%
- Left: brightness -10%
- Right: brightness +10%

Holding repeats after a short delay. The direction is consumed, so it does not
move the player. Values are saved after the controls are released.

The same settings are available as large touch controls under `SETTINGS`.

## Software keyboard

Press **START** during emulation. The keyboard shows four columns at a time.
Move left or right to cross between three banks.

- A: type or toggle the highlighted key
- B: close the keyboard
- L2: toggle the SHIFT symbols page
- `SHF`: on-screen alternative to L2
- `CTRL` and `GRAPH`: latched modifiers

The layout includes printable Japanese-MSX symbols, F1–F5, HOME/CLS, SELECT,
CAPS, STOP, editing keys and cursor keys. `CNT` closes the keyboard.

For a cassette mounted as `.cas`, a common BASIC command is:

```text
RUN"CAS:"
```

This command needs a BIOS/BASIC environment that supports cassette loading;
the included C-BIOS does not provide full cassette BASIC support.

## MSX menu

Press **SELECT** during emulation to open the menu and pause the game.

- `RESUME` returns to the game.
- `LOAD STATE` and `SAVE STATE` use three slots per title and machine model.
- `CHANGE DISK A` and `CHANGE DISK B` mount a `.dsk` image in the chosen drive.
- `REWIND TAPE` rewinds the mounted cassette image.
- `RESET` resets the current emulated machine.
- `QUIT` stops emulation and returns to the launcher.

Press B to go back. A long selected disk name scrolls horizontally.

## Wi-Fi upload

Wi-Fi credentials must first be stored over USB as described in the
installation guide. Open `Wi-Fi UPLOAD`; after connection the watch shows an
HTTP address.

Open the address from a device on the same LAN, choose the destination type,
select one file and upload it. The watch performs an SD read-back SHA-256 check
before replacing an existing file. Leaving the screen stops the Wi-Fi session.

## Save data

- Save states are under `/msx/states` on microSD.
- Each title and machine model has separate slots.
- `.rss` states require the same BIOS and mounted media, including disk swaps.
  v0.1.0 states use an incompatible format.
- Saving uses a temporary file and keeps the previous state as a backup where
  possible.
- Do not remove the card or power off during save/load.

## Display behavior

Game rendering starts at 100% after every cold boot. The USAS demo reached
about 59.4 fps in a steady interval on the watch. Startup and transitions can
be slower. Menus and the software keyboard use native watch overlays.

`60FPS TEST` and `DISPLAY TEST` are developer/diagnostic screens, not games.
BOOT returns to the launcher.

## Troubleshooting

### A cartridge does not start

- Try another machine model or specify the cartridge mapper as described in
  the [core guide](INDEPENDENT_MSX.md).
- Confirm the media is a valid MSX cartridge image.
- C-BIOS compatibility is not complete for every title.
- Do not assume a file renamed to `.rom` is a cartridge.

### A disk or cassette does not start

Use a suitable system BIOS for cassette software. For read-only DSK images,
also provide a compatible WD2793 `DISK.ROM`. Check the filenames in the
[installation guide](INSTALLATION.md).

### Controller is not detected

- Put the 8BitDo Micro in K mode.
- Disconnect it from other hosts.
- Restart the controller and the watch.
- Wait several seconds for `80EL` discovery.

### Wi-Fi does not connect

- Use a reachable 2.4 GHz network.
- Re-enter credentials over USB.
- Ensure the client browser is on the same LAN.
- Reopen `Wi-Fi UPLOAD` after changing credentials.

### A save state does not load

Use the same starting title, machine model, BIOS set, and mounted image
contents. If you changed disks before saving, mount the same disk in each
drive before loading. Save in a different slot to keep an earlier snapshot.

