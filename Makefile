VERSION := 2.0.0
PKG := predatortune
DEB := $(PKG)_$(VERSION)_amd64.deb
BUILD := deb-build/$(PKG)_$(VERSION)

CC := gcc
CFLAGS := -O2 -Wall -Wextra $(shell pkg-config --cflags libadwaita-1)
LDFLAGS := $(shell pkg-config --libs libadwaita-1)

.PHONY: all deb clean install uninstall

all: predatortune predatortune-helper

predatortune: predatortune.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

predatortune-helper: predatortune-helper.c
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
	@mkdir -p $(BUILD)/usr/lib/predatortune
	@cp DEBIAN/* $(BUILD)/DEBIAN/
	@chmod 755 $(BUILD)/DEBIAN/postinst $(BUILD)/DEBIAN/prerm
	@cp predatortune $(BUILD)/usr/bin/
	@chmod 755 $(BUILD)/usr/bin/predatortune
	@cp predatortune-helper $(BUILD)/usr/local/bin/
	@chmod 755 $(BUILD)/usr/local/bin/predatortune-helper
	@cp kmod/predatortune_fan.ko $(BUILD)/usr/lib/predatortune/ 2>/dev/null || true
	@cp 99-predatortune.rules $(BUILD)/etc/udev/rules.d/
	@printf 'predatortune_fan\n' > $(BUILD)/etc/modules-load.d/predatortune.conf
	@cp predatortune-modprobe.conf $(BUILD)/etc/modprobe.d/predatortune.conf
	@cp predatortune.desktop $(BUILD)/usr/share/applications/
	@cp com.predatortune.helper.policy $(BUILD)/usr/share/polkit-1/actions/
	@cp icons/predatortune.svg $(BUILD)/usr/share/icons/hicolor/scalable/apps/
	@dpkg-deb --build $(BUILD) $(DEB)
	@echo "Built: $(DEB)"

install: deb
	sudo dpkg -i $(DEB)

uninstall:
	sudo dpkg -r $(PKG)

clean:
	rm -f predatortune predatortune-helper
	rm -rf deb-build *.deb 2>/dev/null || true
