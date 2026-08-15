VERSION := 2.0.0
PKG := predatortune
DEB := $(PKG)_$(VERSION)_amd64.deb
BUILD := deb-build/$(PKG)_$(VERSION)
SRCDIR := $(BUILD)/usr/src/linuwu-sense-$(VERSION)

CC := gcc
CFLAGS := -O2 -Wall -Wextra $(shell pkg-config --cflags libadwaita-1 2>/dev/null)
LDFLAGS := $(shell pkg-config --libs libadwaita-1 2>/dev/null)

# The GUI window needs libadwaita-1-dev. The daemon, tray and kernel module
# do not, so a machine without it can still build a package that keeps the
# fans under control — it just has no window.
HAVE_ADW := $(shell pkg-config --exists libadwaita-1 2>/dev/null && echo yes)

.PHONY: all gui deb clean install uninstall

all: gui predatortune-helper predatortune-daemon

ifeq ($(HAVE_ADW),yes)
gui: predatortune
else
gui:
	@echo "NOTE: libadwaita-1-dev not installed, skipping the GUI window."
	@echo "      sudo apt install libadwaita-1-dev   then re-run make"
endif

predatortune: predatortune.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

predatortune-helper: predatortune-helper.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

predatortune-daemon: predatortune-daemon.c
	$(CC) -O2 -Wall -Wextra -o $@ $<

deb: all
	@mkdir -p $(BUILD)/DEBIAN
	@mkdir -p $(BUILD)/usr/bin
	@mkdir -p $(BUILD)/usr/local/bin
	@mkdir -p $(BUILD)/usr/share/applications
	@mkdir -p $(BUILD)/usr/share/polkit-1/actions
	@mkdir -p $(BUILD)/usr/share/icons/hicolor/scalable/apps
	@mkdir -p $(BUILD)/etc/udev/rules.d
	@mkdir -p $(BUILD)/etc/modules-load.d
	@mkdir -p $(BUILD)/etc/modprobe.d
	@mkdir -p $(BUILD)/usr/lib/tmpfiles.d
	@mkdir -p $(BUILD)/etc/predatortune
	@mkdir -p $(BUILD)/etc/xdg/autostart
	@mkdir -p $(BUILD)/lib/systemd/system
	@mkdir -p $(SRCDIR)
	@cp DEBIAN/control DEBIAN/conffiles $(BUILD)/DEBIAN/
	@cp DEBIAN/postinst DEBIAN/prerm DEBIAN/postrm $(BUILD)/DEBIAN/
	@chmod 755 $(BUILD)/DEBIAN/postinst $(BUILD)/DEBIAN/prerm $(BUILD)/DEBIAN/postrm
	@if [ -x predatortune ]; then \
		cp predatortune $(BUILD)/usr/bin/; \
		chmod 755 $(BUILD)/usr/bin/predatortune; \
		cp predatortune.desktop $(BUILD)/usr/share/applications/; \
	else \
		echo "NOTE: packaging without the GUI window (libadwaita-1-dev missing)"; \
	fi
	@cp predatortune-daemon $(BUILD)/usr/bin/
	@cp predatortune-tray.py $(BUILD)/usr/bin/predatortune-tray
	@chmod 755 $(BUILD)/usr/bin/predatortune-daemon
	@chmod 755 $(BUILD)/usr/bin/predatortune-tray
	@cp predatortune-helper $(BUILD)/usr/local/bin/
	@chmod 755 $(BUILD)/usr/local/bin/predatortune-helper
# DKMS source, not a prebuilt .ko: rebuilt and re-signed for every kernel.
# linuwu_sense replaces the in-tree acer_wmi and covers fan speed, the battery
# limiter and the keyboard zones, so our own fan module is no longer built.
	@cp linuwu/linuwu_sense.c linuwu/Makefile linuwu/dkms.conf linuwu/LICENSE $(SRCDIR)/
	@cp 99-predatortune.rules $(BUILD)/etc/udev/rules.d/
	@printf 'linuwu_sense\n' > $(BUILD)/etc/modules-load.d/predatortune.conf
	@printf '# linuwu_sense is a fork of this driver and cannot coexist with it.\nblacklist acer_wmi\n' \
		> $(BUILD)/etc/modprobe.d/predatortune.conf
	@cp predatortune-tmpfiles.conf $(BUILD)/usr/lib/tmpfiles.d/predatortune.conf
	@cp fan.conf $(BUILD)/etc/predatortune/fan.conf
	@cp predatortune-daemon.service $(BUILD)/lib/systemd/system/
	@cp predatortune-resume.service $(BUILD)/lib/systemd/system/
	@cp predatortune-tray.desktop $(BUILD)/etc/xdg/autostart/
	@cp com.predatortune.helper.policy $(BUILD)/usr/share/polkit-1/actions/
	@cp icons/predatortune.svg $(BUILD)/usr/share/icons/hicolor/scalable/apps/
	@dpkg-deb --build $(BUILD) $(DEB)
	@echo "Built: $(DEB)"

install: deb
	sudo dpkg -i $(DEB)

uninstall:
	sudo dpkg -r $(PKG)

clean:
	rm -f predatortune predatortune-helper predatortune-daemon
	rm -rf deb-build *.deb 2>/dev/null || true
