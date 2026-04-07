VERSION := 1.0.0
PKG := predatortune
DEB := $(PKG)_$(VERSION)_all.deb
BUILD := deb-build/$(PKG)_$(VERSION)

.PHONY: deb clean install uninstall

deb:
	@mkdir -p $(BUILD)/DEBIAN
	@mkdir -p $(BUILD)/usr/bin
	@mkdir -p $(BUILD)/usr/lib/predatortune
	@mkdir -p $(BUILD)/usr/local/bin
	@mkdir -p $(BUILD)/usr/share/applications
	@mkdir -p $(BUILD)/usr/share/polkit-1/actions
	@mkdir -p $(BUILD)/usr/share/icons/hicolor/scalable/apps
	@cp DEBIAN/* $(BUILD)/DEBIAN/
	@chmod 755 $(BUILD)/DEBIAN/postinst $(BUILD)/DEBIAN/prerm
	@printf '#!/bin/bash\nexec python3 /usr/lib/predatortune/predatortune.py "$$@"\n' > $(BUILD)/usr/bin/predatortune
	@chmod 755 $(BUILD)/usr/bin/predatortune
	@cp predatortune.py $(BUILD)/usr/lib/predatortune/
	@chmod 755 $(BUILD)/usr/lib/predatortune/predatortune.py
	@cp predatortune-helper $(BUILD)/usr/local/bin/
	@chmod 755 $(BUILD)/usr/local/bin/predatortune-helper
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
	rm -r deb-build *.deb 2>/dev/null || true
