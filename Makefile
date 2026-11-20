INSTALL_BIN := jarvis
all := libjarvis jarvis

libjarvis-obj-y := src/jarvis-json.o src/jarvis-ontology.o src/jarvis-solver.o src/jarvis-voice.o src/jarvis-adapters.o
jarvis-obj-y := src/jarvis-cli-adapter.o
LDLIBS-libjarvis := -lmm -lxylem -lstele -lcorm -lm
LDLIBS-jarvis := -Llib -ljarvis -lmm -lxylem -lstele -lcorm -lm
LDFLAGS-libjarvis-Linux := -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${HOME}/.local/lib -Wl,-rpath,${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/../libxylem/lib -Wl,-rpath,${CURDIR}/../libstele/lib -Wl,-rpath,${CURDIR}/../libcorm/lib
LDFLAGS-jarvis-Linux := $(LDFLAGS-libjarvis-Linux)
CFLAGS-BIN-Windows :=

CFLAGS := -g -O2 -Iinclude -I../libmm/include -I../libxylem/include -I../libstele/include -I../libcorm/include -Isrc -Isrc/audio -Isrc/vad -Isrc/whisper -Isrc/server -Isrc/tts -Isrc/voice
LDFLAGS := -L../libmm/lib -L../libxylem/lib -L../libstele/lib -L../libcorm/lib

WASM_SRCS := npm/wasm/jarvis_wasm.c
WASM_EXTRA_CFLAGS := -Iinclude -I../libmm/include -I../libxylem/include -I../libstele/include

include ../mk/include.mk

WM_MODS := mods/sway/mod_sway.so mods/hyprland/mod_hyprland.so mods/tts/mod_tts.so
WHISPER_LIB := $(HOME)/.local/lib/libwhisper.so.1
GGML_LIB := $(HOME)/.local/lib/libggml.so.0
JARVISD_OBJS := src/jarvisd_main.o src/audio/audio.o src/vad/vad.o src/whisper/whisper_mod.o src/server/server.o src/tts/tts.o src/voice/voice.o
JARVISD_BIN := $(if $(wildcard $(WHISPER_LIB)),bin/jarvisd,)

all: $(WM_MODS) $(JARVISD_BIN)

mods/sway/mod_sway.so: mods/sway/mod_sway.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/sway/mod_sway.c $(LDFLAGS) -lxylem -lstele -lcorm

mods/hyprland/mod_hyprland.so: mods/hyprland/mod_hyprland.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/hyprland/mod_hyprland.c $(LDFLAGS) -lxylem -lstele -lcorm

mods/tts/mod_tts.so: mods/tts/mod_tts.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/tts/mod_tts.c $(LDFLAGS) -lxylem -ldl -lpthread -lm

bin/jarvisd: $(JARVISD_OBJS) lib/libjarvis.so
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(JARVISD_OBJS) $(WHISPER_LIB) $(GGML_LIB) -Llib -ljarvis $(LDFLAGS) -lmm -lxylem -lstele -lcorm -lpthread -ldl -lm $(LDFLAGS-libjarvis-Linux)

test: all
	$(MAKE) -C tests test

${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc: jarvis.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 jarvis.pc $@

${DESTDIR}${PREFIX}/lib/mod_sway.so: mods/sway/mod_sway.so
	install -d ${DESTDIR}${PREFIX}/lib
	install -m 755 mods/sway/mod_sway.so $@

${DESTDIR}${PREFIX}/lib/mod_hyprland.so: mods/hyprland/mod_hyprland.so
	install -d ${DESTDIR}${PREFIX}/lib
	install -m 755 mods/hyprland/mod_hyprland.so $@

${DESTDIR}${PREFIX}/lib/mod_tts.so: mods/tts/mod_tts.so
	install -d ${DESTDIR}${PREFIX}/lib
	install -m 755 mods/tts/mod_tts.so $@

install-extra: ${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc ${DESTDIR}${PREFIX}/lib/mod_sway.so ${DESTDIR}${PREFIX}/lib/mod_hyprland.so ${DESTDIR}${PREFIX}/lib/mod_tts.so
	@mkdir -p $(HOME)/.local/lib $(HOME)/.local/bin $(HOME)/.config/jarvis
	@install -m 755 mods/tts/mod_tts.so $(HOME)/.local/lib/mod_tts.so
	@install -m 644 data/ontology.json $(HOME)/.config/jarvis/ontology.json
	@if [ -f bin/jarvis ]; then \
		install -m 755 bin/jarvis $(HOME)/.local/bin/jarvis; \
	fi
	@if [ -f bin/jarvisd ]; then \
		install -d ${DESTDIR}${PREFIX}/bin; \
		install -m 755 bin/jarvisd ${DESTDIR}${PREFIX}/bin/jarvisd; \
		install -m 755 bin/jarvisd $(HOME)/.local/bin/jarvisd; \
		ln -sf jarvisd $(HOME)/.local/bin/dwm-whisperd; \
	fi

install: install-extra

clean: clean-tests
clean-tests:
	rm -f $(WM_MODS) bin/jarvisd
	$(MAKE) -C tests clean

# Optional Node.js & WebAssembly bindings
NPM ?= npm

sync-npm-headers:
	mkdir -p npm/include/ttypt
	cp include/ttypt/jarvis.h npm/include/ttypt/jarvis.h
	cp include/ttypt/jarvis-types.h npm/include/ttypt/jarvis-types.h
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
