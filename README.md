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
- Only `off` (`01`) and `green` (`03`) are confirmed values.

## How it works

The LED is controlled by the SMC key `ACLC`. macOS writes it whenever the
charging state changes. `magled` listens for power-source and wake
notifications, checks the key, and rewrites it only if it's wrong. Each fix is
logged to `/var/log/magled.log`.

## Versioning

The version is the `MAGLED_VERSION` line near the top of `magled.c`. **Bump it
with every change**, before building and installing, so `magled --version`
always tells you which code is running. Use major.minor.patch: patch for
fixes and small tweaks, minor for new behaviour, major for big reworks or big
additions.

## Author

Mark Schlosser

## License

MIT. See [LICENSE](LICENSE).
