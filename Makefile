PREFIX ?= /usr/local
PLIST = /Library/LaunchDaemons/com.local.magled.plist
MODE ?= off

magled: magled.c
	clang -O2 -Wall -Wextra -o $@ $< -framework IOKit

# Installs the binary only.
install: magled
	sudo install -d $(PREFIX)/bin
	sudo install -m 755 magled $(PREFIX)/bin/magled

# Runs magled at boot with MODE (off, green, or hex).
# Run again with a different MODE to switch.
enable: install
	sed 's/@MODE@/$(MODE)/' com.local.magled.plist > magled.plist.tmp
	sudo install -m 644 -o root -g wheel magled.plist.tmp $(PLIST)
	rm -f magled.plist.tmp
	sudo launchctl bootout system $(PLIST) 2>/dev/null || true
	sudo launchctl bootstrap system $(PLIST)

# Stops the daemon and lets macOS control the LED again.
disable:
	sudo launchctl bootout system $(PLIST) 2>/dev/null || true
	sudo rm -f $(PLIST)

uninstall: disable
	sudo rm -f $(PREFIX)/bin/magled

clean:
	rm -f magled

.PHONY: install enable disable uninstall clean
