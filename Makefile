INSTALL_BIN := jarvis
all := libjarvis jarvis

libjarvis-obj-y := src/jarvis-json.o src/jarvis-ontology.o src/jarvis-voice.o
jarvis-obj-y :=
LDLIBS-libjarvis := -lm
LDLIBS-jarvis := -Llib -ljarvis -lm
LDFLAGS-libjarvis-Linux := -Wl,-rpath,'$$ORIGIN'
LDFLAGS-jarvis-Linux := -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,'$$ORIGIN'
CFLAGS-BIN-Windows :=

CFLAGS := -g -O2 -Iinclude
LDFLAGS :=

WASM_SRCS := npm/wasm/jarvis_wasm.c
WASM_EXTRA_CFLAGS := -Iinclude -I../libstele/include

include ../mk/include.mk

WM_MODS := mods/sway/ren-sway.so mods/hyprland/ren-hyprland.so \
	mods/x11/ren-x11.so mods/firefox/ren-firefox.so \
	mods/desktop/ren-desktop.so mods/ren-jarvis/ren-jarvis.so
whispercpp := submodules/whisper.cpp/build
WHISPER_A := $(whispercpp)/src/libwhisper.a
JARVISD_BIN := bin/jarvisd

VULKAN_SDK := third_party/1.4.328.1
VULKAN_SDK_URL := https://sdk.lunarg.com/sdk/download/1.4.328.1/linux/vulkansdk-linux-x86_64-1.4.328.1.tar.xz

$(VULKAN_SDK)/x86_64/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake:
	mkdir -p third_party
	(curl -fsSL "$(VULKAN_SDK_URL)" -o third_party/vulkan-sdk.tar.xz || wget -qO third_party/vulkan-sdk.tar.xz "$(VULKAN_SDK_URL)")
	tar -xf third_party/vulkan-sdk.tar.xz -C third_party

$(whispercpp)/Makefile: $(VULKAN_SDK)/x86_64/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake
	@echo "[INFO] Vulkan SDK extracted at $(VULKAN_SDK)"
	@mkdir -p $(whispercpp)
	cd $(whispercpp) && cmake .. \
		-DCMAKE_PREFIX_PATH=$(CURDIR)/$(VULKAN_SDK)/x86_64 \
		-DGGML_VULKAN=ON \
		-DBUILD_SHARED_LIBS=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DWHISPER_BUILD_EXAMPLES=OFF \
		-DWHISPER_BUILD_TESTS=OFF \
		-DWHISPER_BUILD_SERVER=OFF

all: $(WM_MODS) $(JARVISD_BIN)

mods/sway/ren-sway.so: mods/sway/ren-sway.c
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/sway/ren-sway.c $(LDFLAGS) -L../axil-auren/lib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/hyprland/ren-hyprland.so: mods/hyprland/ren-hyprland.c
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/hyprland/ren-hyprland.c $(LDFLAGS) -L../axil-auren/lib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/x11/ren-x11.so: mods/x11/ren-x11.c mods/x11/x11_core.h include/ttypt/desktop-core.h
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/x11/ren-x11.c $(LDFLAGS) -L../axil-auren/lib -lren -lX11 -ldl -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/firefox/ren-firefox.so: mods/firefox/ren-firefox.c mods/firefox/firefox_core.h
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/firefox/ren-firefox.c $(LDFLAGS) -L../axil-auren/lib -lren -ldl -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/desktop/ren-desktop.so: mods/desktop/ren-desktop.c include/ttypt/desktop-core.h
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/desktop/ren-desktop.c $(LDFLAGS) -L../axil-auren/lib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

mods/ren-jarvis/ren-jarvis.so: mods/ren-jarvis/ren-jarvis.c
	@mkdir -p mods/ren-jarvis
	$(CC) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/ren-jarvis/ren-jarvis.c $(LDFLAGS) -L../axil-auren/lib -lren -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'

$(WHISPER_A): $(whispercpp)/Makefile
	$(MAKE) -C $(whispercpp) -j4 whisper

bin/jarvisd: src/jarvisd.c src/jarvis-tts.c src/jarvis-tts.h $(WHISPER_A) lib/libjarvis.so
	@mkdir -p bin
	$(CC) -Iinclude -Isrc -I$(whispercpp)/../include -I$(whispercpp)/../ggml/include $(CFLAGS) -o $@ src/jarvisd.c src/jarvis-tts.c \
		$(WHISPER_A) \
		$(whispercpp)/ggml/src/libggml.a \
		$(whispercpp)/ggml/src/ggml-vulkan/libggml-vulkan.a \
		$(whispercpp)/ggml/src/libggml-cpu.a \
		$(whispercpp)/ggml/src/libggml-base.a \
		-Llib -ljarvis $(LDFLAGS) -lvulkan -lstdc++ -lgomp -lpthread -ldl -lm $(LDFLAGS-jarvis-${uname})

test: all
	$(MAKE) -C tests test

${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc: jarvis.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 jarvis.pc $@

${DESTDIR}${PREFIX}/lib/ren/ren-sway.so: mods/sway/ren-sway.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/sway/ren-sway.so $@

${DESTDIR}${PREFIX}/lib/ren/ren-hyprland.so: mods/hyprland/ren-hyprland.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/hyprland/ren-hyprland.so $@

${DESTDIR}${PREFIX}/lib/ren/ren-x11.so: mods/x11/ren-x11.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/x11/ren-x11.so $@

${DESTDIR}${PREFIX}/lib/ren/ren-firefox.so: mods/firefox/ren-firefox.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/firefox/ren-firefox.so $@

${DESTDIR}${PREFIX}/lib/ren/ren-desktop.so: mods/desktop/ren-desktop.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/desktop/ren-desktop.so $@

${DESTDIR}${PREFIX}/lib/ren/ren-jarvis.so: mods/ren-jarvis/ren-jarvis.so
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/ren-jarvis/ren-jarvis.so $@

install-extra: ${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc ${DESTDIR}${PREFIX}/lib/ren/ren-sway.so ${DESTDIR}${PREFIX}/lib/ren/ren-hyprland.so ${DESTDIR}${PREFIX}/lib/ren/ren-x11.so ${DESTDIR}${PREFIX}/lib/ren/ren-firefox.so ${DESTDIR}${PREFIX}/lib/ren/ren-desktop.so ${DESTDIR}${PREFIX}/lib/ren/ren-jarvis.so
	@rm -f ${DESTDIR}${PREFIX}/lib/mod_sway.so ${DESTDIR}${PREFIX}/lib/mod_hyprland.so ${DESTDIR}${PREFIX}/lib/mod_tts.so ${DESTDIR}${PREFIX}/lib/ren/mod_jarvis.so $(HOME)/.local/lib/mod_sway.so $(HOME)/.local/lib/mod_hyprland.so $(HOME)/.local/lib/mod_tts.so
	@rm -f ${DESTDIR}${PREFIX}/lib/jarvis/jarvis-*.so $(HOME)/.local/lib/jarvis/jarvis-*.so
	@rmdir ${DESTDIR}${PREFIX}/lib/jarvis $(HOME)/.local/lib/jarvis 2>/dev/null || true
	@mkdir -p $(HOME)/.local/lib $(HOME)/.local/bin $(HOME)/.config/jarvis
	@install -d ${DESTDIR}${PREFIX}/share/jarvis
	@install -m 644 data/ontology.json ${DESTDIR}${PREFIX}/share/jarvis/ontology.json
	@if [ -f data/99-jarvis-aec.conf ]; then \
		install -m 644 data/99-jarvis-aec.conf ${DESTDIR}${PREFIX}/share/jarvis/99-jarvis-aec.conf; \
	fi
	@install -m 644 data/ontology.json $(HOME)/.config/jarvis/ontology.json
	@if [ -f bin/jarvis ]; then \
		install -m 755 bin/jarvis $(HOME)/.local/bin/jarvis; \
	fi
	@if [ -f scripts/edge-tts-pcm ]; then \
		install -d ${DESTDIR}${PREFIX}/bin; \
		install -m 755 scripts/edge-tts-pcm ${DESTDIR}${PREFIX}/bin/edge-tts-pcm; \
		install -m 755 scripts/edge-tts-pcm $(HOME)/.local/bin/edge-tts-pcm; \
	fi
	@if [ -f bin/jarvisd ]; then \
		install -d ${DESTDIR}${PREFIX}/bin; \
		install -m 755 bin/jarvisd ${DESTDIR}${PREFIX}/bin/jarvisd; \
		install -m 755 bin/jarvisd $(HOME)/.local/bin/jarvisd; \
		ln -sf jarvisd $(HOME)/.local/bin/dwm-whisperd; \
	fi

install: install-extra install-services

.PHONY: install-services
install-services:
	@if [ "${uname}" = "Linux" ] || [ -d /run/systemd/system ]; then \
		echo "[INFO] Installing systemd services for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/lib/systemd/user; \
		sed 's|@PREFIX@|${PREFIX}|g' services/systemd/jarvisd.service > ${DESTDIR}${PREFIX}/lib/systemd/user/jarvisd.service; \
		if [ -d $(HOME)/.config/systemd/user ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; then \
			mkdir -p $(HOME)/.config/systemd/user; \
			sed 's|@PREFIX@|${PREFIX}|g' services/systemd/jarvisd.service > $(HOME)/.config/systemd/user/jarvisd.service; \
		fi; \
	fi
	@if [ -d /etc/init.d ] || [ -x /sbin/openrc-run ] || [ "${distro}" = "alpine" ] || [ "${distro}" = "gentoo" ]; then \
		echo "[INFO] Installing OpenRC service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/examples/openrc; \
		sed 's|@PREFIX@|${PREFIX}|g' services/openrc/jarvisd > ${DESTDIR}${PREFIX}/share/examples/openrc/jarvisd; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/openrc/jarvisd; \
		if [ -w /etc/init.d ]; then \
			install -m 755 ${DESTDIR}${PREFIX}/share/examples/openrc/jarvisd /etc/init.d/jarvisd; \
		fi; \
	fi
	@if [ "${uname}" = "FreeBSD" ] || [ "${uname}" = "NetBSD" ] || [ "${uname}" = "DragonFly" ]; then \
		echo "[INFO] Installing FreeBSD rc.d service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/etc/rc.d ${DESTDIR}${PREFIX}/share/examples/freebsd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/freebsd/jarvisd > ${DESTDIR}${PREFIX}/share/examples/freebsd/jarvisd; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/freebsd/jarvisd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/freebsd/jarvisd > ${DESTDIR}${PREFIX}/etc/rc.d/jarvisd; \
		chmod 755 ${DESTDIR}${PREFIX}/etc/rc.d/jarvisd; \
	elif [ "${uname}" = "OpenBSD" ]; then \
		echo "[INFO] Installing OpenBSD rc.d service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/examples/rc.d; \
		sed 's|@PREFIX@|${PREFIX}|g' services/rc.d/jarvisd > ${DESTDIR}${PREFIX}/share/examples/rc.d/jarvisd; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/rc.d/jarvisd; \
		if [ -w /etc/rc.d ]; then \
			install -m 755 ${DESTDIR}${PREFIX}/share/examples/rc.d/jarvisd /etc/rc.d/jarvisd; \
		fi; \
	elif [ "${uname}" = "Darwin" ]; then \
		echo "[INFO] Installing launchd service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/launchd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.jarvisd.plist > ${DESTDIR}${PREFIX}/share/launchd/pt.tty.jarvisd.plist; \
		if [ -d $(HOME)/Library/LaunchAgents ]; then \
			sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.jarvisd.plist > $(HOME)/Library/LaunchAgents/pt.tty.jarvisd.plist; \
		fi; \
	elif [ "${SYS}" = "Windows" ] || [ "${uname}" = "MinGW64" ] || [ "${uname}" = "Msys" ] || [ "${uname}" = "MingW" ]; then \
		echo "[INFO] Installing Windows services for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/windows; \
		sed 's|@PREFIX@|${PREFIX}|g' services/windows/jarvisd.xml > ${DESTDIR}${PREFIX}/share/windows/jarvisd.xml; \
		install -m 755 services/windows/install-service.ps1 ${DESTDIR}${PREFIX}/share/windows/install-jarvisd-service.ps1; \
	fi
	@if [ -f services/xdg/jarvisd.desktop ]; then \
		install -d ${DESTDIR}${PREFIX}/etc/xdg/autostart ${DESTDIR}${PREFIX}/share/applications; \
		sed 's|@PREFIX@|${PREFIX}|g' services/xdg/jarvisd.desktop > ${DESTDIR}${PREFIX}/etc/xdg/autostart/jarvisd.desktop; \
		sed 's|@PREFIX@|${PREFIX}|g' services/xdg/jarvisd.desktop > ${DESTDIR}${PREFIX}/share/applications/jarvisd.desktop; \
		if [ -d $(HOME)/.config/autostart ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; then \
			mkdir -p $(HOME)/.config/autostart; \
			sed 's|@PREFIX@|${PREFIX}|g' services/xdg/jarvisd.desktop > $(HOME)/.config/autostart/jarvisd.desktop; \
		fi; \
	fi

clean: clean-tests
clean-tests:
	rm -f $(WM_MODS) bin/jarvisd src/*.o src/*.o.d
	$(MAKE) -C tests clean

# Optional Node.js & WebAssembly bindings
NPM ?= npm

sync-npm-headers:
	mkdir -p npm/include/ttypt
	cp include/ttypt/jarvis.h npm/include/ttypt/jarvis.h
	cp include/ttypt/jarvis-types.h npm/include/ttypt/jarvis-types.h
	cp include/ttypt/desktop-core.h npm/include/ttypt/desktop-core.h
	cp jarvis.pc npm/jarvis.pc

npm-build: all sync-npm-headers
	@if command -v $(NPM) >/dev/null 2>&1; then \
		(cd npm && $(NPM) run build); \
	else \
		echo "Skipping npm build — $(NPM) not found"; \
	fi

npm-test: npm-build
	@if command -v $(NPM) >/dev/null 2>&1; then \
		(cd npm && $(NPM) test); \
	fi

.PHONY: all test install clean-tests sync-npm-headers npm-build npm-test
