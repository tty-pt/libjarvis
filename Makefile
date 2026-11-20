INSTALL_BIN := jarvis
all := libjarvis jarvis

libjarvis-obj-y := src/jarvis-json.o src/jarvis-ontology.o src/jarvis-solver.o src/jarvis-voice.o src/jarvis-adapters.o
jarvis-obj-y := src/jarvis-cli-adapter.o
LDLIBS-libjarvis := -lmm -lxylem -lstele -lcorm -lm
LDLIBS-jarvis := -Llib -ljarvis -lmm -lxylem -lstele -lcorm -lm
LDFLAGS-libjarvis-Linux := -Wl,-rpath,${CURDIR}/lib -Wl,-rpath,${HOME}/.local/lib -Wl,-rpath,${CURDIR}/../libmm/lib -Wl,-rpath,${CURDIR}/../libxylem/lib -Wl,-rpath,${CURDIR}/../libstele/lib -Wl,-rpath,${CURDIR}/../libcorm/lib
LDFLAGS-jarvis-Linux := $(LDFLAGS-libjarvis-Linux)
CFLAGS-BIN-Windows :=

CFLAGS := -g -O2 -Iinclude -I../libmm/include -I../libxylem/include -I../libstele/include -I../libcorm/include
LDFLAGS := -L../libmm/lib -L../libxylem/lib -L../libstele/lib -L../libcorm/lib

WASM_SRCS := npm/wasm/jarvis_wasm.c
WASM_EXTRA_CFLAGS := -Iinclude -I../libmm/include -I../libxylem/include -I../libstele/include

include ../mk/include.mk

WM_MODS := mods/sway/mod_sway.so mods/hyprland/mod_hyprland.so mods/tts/mod_tts.so
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

mods/sway/mod_sway.so: mods/sway/mod_sway.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/sway/mod_sway.c $(LDFLAGS) -lxylem -lstele -lcorm

mods/hyprland/mod_hyprland.so: mods/hyprland/mod_hyprland.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/hyprland/mod_hyprland.c $(LDFLAGS) -lxylem -lstele -lcorm

mods/tts/mod_tts.so: mods/tts/mod_tts.c include/ttypt/jarvis-types.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ mods/tts/mod_tts.c $(LDFLAGS) -lxylem -ldl -lpthread -lm

$(whispercpp)/Makefile:
	@mkdir -p $(whispercpp)
	cd $(whispercpp) && cmake .. \
		-DCMAKE_PREFIX_PATH=/home/quirinpa/site/external/axil-auren/third_party/1.4.328.1/x86_64 \
		-DGGML_VULKAN=ON \
		-DBUILD_SHARED_LIBS=OFF \
		-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
		-DWHISPER_BUILD_EXAMPLES=OFF \
		-DWHISPER_BUILD_TESTS=OFF \
		-DWHISPER_BUILD_SERVER=OFF

$(WHISPER_A): $(whispercpp)/Makefile
	$(MAKE) -C $(whispercpp) -j4 whisper

bin/jarvisd: src/jarvisd.c $(WHISPER_A) lib/libjarvis.so
	@mkdir -p bin
	$(CC) -Iinclude -Isrc -I$(whispercpp)/../include -I$(whispercpp)/../ggml/include $(CFLAGS) -o $@ src/jarvisd.c \
		$(WHISPER_A) \
		$(whispercpp)/ggml/src/libggml.a \
		$(whispercpp)/ggml/src/ggml-vulkan/libggml-vulkan.a \
		$(whispercpp)/ggml/src/libggml-cpu.a \
		$(whispercpp)/ggml/src/libggml-base.a \
		-Llib -ljarvis $(LDFLAGS) -lmm -lxylem -lstele -lcorm -lvulkan -lstdc++ -lgomp -lpthread -ldl -lm $(LDFLAGS-libjarvis-Linux)

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
