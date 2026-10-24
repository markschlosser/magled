# magled

Turns off the LED on a MacBook's MagSafe 3 connector (the magnetic plug on the
charging cable), and keeps it off.

A single C file, no dependencies, no UI. It runs as a tiny background service
that does nothing until macOS changes the LED (plugging in, waking, charging
state), then sets it back.

> **Warning:** this is unofficial. It writes to the Mac's System Management
> Controller (SMC) through an undocumented interface. It has only been tested on
> an Apple Silicon MacBook Air running macOS 27.0.1. Use at your own risk.

## Requirements

- A MacBook with a MagSafe 3 port (Apple Silicon). Tested on a MacBook Air
  running macOS 27.0.1.
- Xcode Command Line Tools, for `clang` and `make`. The full Xcode app is not
  needed. Install with `xcode-select --install`.
- An admin account: installing and running it uses `sudo`.

## Install

One command does everything (build, install to `/usr/local/bin`, start the
service, and run it at every boot):

```sh
make enable              # keep the LED off
make enable MODE=green   # or keep it green
```

After changing the code, run the same command again to rebuild and restart.
Include the same `MODE=` as before; without it, the mode goes back to `off`.

Other targets, only if you don't want the boot service:

```sh
make           # just build ./magled (see "Run by hand")
make install   # just copy it to /usr/local/bin
```

## Uninstall

```sh
make disable    # stop it; the LED goes back to normal
make uninstall  # also remove /usr/local/bin/magled
```

## Run by hand

```sh
sudo ./magled        # keep the LED off until Ctrl-C
sudo ./magled green  # or green; any raw hex value also works
```

Stopping it puts back the colour macOS last chose.

`magled --version` prints the installed version. The service also logs its
version to `/var/log/magled.log` each time it starts.

## Known limits

- The LED still flashes green briefly when you plug in. The SMC ignores
  writes for a moment while the charger connects, so software can't prevent it.
- While the Mac is asleep, the LED comes back on. A few seconds into sleep the
  Mac renegotiates the USB-C power contract from 20 V down to 5 V, and the SMC
  treats that like a fresh plug-in. Nothing can run during sleep to stop it;
  `magled` turns it off again within milliseconds of every wake, including the
  short maintenance wakes. Those wakes can be minutes apart, so expect the LED
  to be green for most of the time the Mac is asleep.
- Writing a different value doesn't help: `01` and `81` were both reset during
  sleep. The only software fix would be to stop the Mac from idle-sleeping
  while on power, which `magled` deliberately doesn't do.
- If the green during sleep bothers you: charge through one of the Mac's USB-C
  ports with a plain USB-C cable (no LED at all), or cover the connector LED.

## LED values

`ACLC` is one byte. Any of these can be passed as a hex value, e.g.
`sudo ./magled 04`. Tested on a MacBook Air while awake:

| Value | LED |
|---|---|
| `00` | macOS controls it. Reads back as its current choice (e.g. `03`) at once. |
| `01` | off |
| `02` | accepted, but the LED stays as it was. Meaning unknown. |
| `03` | green (what macOS sets when fully charged) |
| `04` | amber, solid |
| `05` | amber, blinks a few times, then off |
| `06` | amber, blinks for longer, goes off, then blinks again |
| `07` | amber, blinks continuously |
| `81` | stored as `01` (off). The high bit is ignored. |

## How it works

The LED is controlled by the SMC key `ACLC`. Reading it needs no special
rights; writing it needs root. macOS writes it whenever the charging state
changes, and the SMC resets it whenever the charger (re)connects, including at
plug-in and a few seconds into every sleep. `magled` listens for power-source
and wake notifications, checks the key, and rewrites it only if it's wrong.
After plug-in or a full wake it keeps checking for 5 seconds, because the SMC
resets the LED in two short bursts. The brief maintenance ("dark") wakes don't
send wake notifications to programs, but they do send a power-source
notification, which is how `magled` catches them. Each fix is logged to
`/var/log/magled.log`.

## Versioning

The version is the `MAGLED_VERSION` line near the top of `magled.c`. **Bump it
with every change to the code or build files** (`magled.c`, `Makefile`, the
plist), before building and installing. Edits to the README alone don't
change the version. That way `magled --version` always tells you which code
is running. Use major.minor.patch: patch for fixes and small tweaks, minor for
new behaviour, major for big reworks or big additions.

## Author

Mark Schlosser

## License

MIT. See [LICENSE](LICENSE).
