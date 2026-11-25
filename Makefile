uname := $(shell test "${cross}" = "" && uname || echo ${cross})
uname != test "${cross}" = "" && uname || echo ${cross}

INSTALL_BIN := jarvis
all := libjarvis jarvis

libjarvis-obj-y := src/jarvis-json.o src/jarvis-ontology.o src/jarvis-voice.o src/jarvis-ren-ipc.o
libjarvis-obj-y-Linux := src/jarvis-audio-linux.o src/jarvis-platform-linux.o
libjarvis-obj-y-OpenBSD := src/jarvis-audio-linux.o src/jarvis-platform-linux.o
libjarvis-obj-y-FreeBSD := src/jarvis-audio-linux.o src/jarvis-platform-linux.o
libjarvis-obj-y-NetBSD := src/jarvis-audio-linux.o src/jarvis-platform-linux.o
libjarvis-obj-y-DragonFly := src/jarvis-audio-linux.o src/jarvis-platform-linux.o
libjarvis-obj-y-Darwin := src/jarvis-audio-macos.o src/jarvis-platform-macos.o
libjarvis-obj-y-MinGW64 := src/jarvis-audio-win32.o src/jarvis-platform-win32.o
libjarvis-obj-y-Msys := src/jarvis-audio-win32.o src/jarvis-platform-win32.o
libjarvis-obj-y-MingW := src/jarvis-audio-win32.o src/jarvis-platform-win32.o
jarvis-obj-y :=

LDLIBS-libjarvis := -lstele -lm
LDLIBS-libjarvis-Darwin := -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework CoreFoundation -framework ApplicationServices -framework Carbon
LDLIBS-libjarvis-Windows := -lws2_32 -lwinmm -lole32 -luuid
LDLIBS-jarvis := -Llib -ljarvis -lstele -lm
LDLIBS-jarvis-Windows := -lws2_32
LDFLAGS-libjarvis-Linux := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN'
LDFLAGS-jarvis-Linux := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN/../lib' -Wl,-rpath,'$$ORIGIN'
LDFLAGS-libjarvis-Darwin := -Wl,-rpath,@loader_path
LDFLAGS-jarvis-Darwin := -Wl,-rpath,@loader_path/../lib -Wl,-rpath,@loader_path
LDFLAGS-libjarvis-Windows := -Wl,--export-all-symbols
CFLAGS-BIN-Windows :=

CFLAGS := -g -O2 -Iinclude -I../libstele/include
LDFLAGS := -L${CURDIR}/../libstele/lib -L${CURDIR}/../axil-auren/lib -L${CURDIR}/../libxylem/lib

WASM_SRCS := npm/wasm/jarvis_wasm.c
WASM_EXTRA_CFLAGS := -Iinclude -I../libstele/include

include ../mk/include.mk

MOD_RPATH_Linux := -Wl,--disable-new-dtags -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN/../../lib'
MOD_RPATH_Unix := ${MOD_RPATH_Linux}
MOD_RPATH_Darwin := -Wl,-rpath,@loader_path/.. -Wl,-rpath,@loader_path/../../lib
MOD_RPATH_Windows :=
MOD_DEPS_LDFLAGS := -lren -lxylem -lstele

WM_MODS-Linux := mods/sway/ren-sway.$(SO) mods/hyprland/ren-hyprland.$(SO) \
	mods/x11/ren-x11.$(SO) mods/firefox/ren-firefox.$(SO) \
	mods/desktop/ren-desktop.$(SO) mods/ren-jarvis/ren-jarvis.$(SO)
WM_MODS-Darwin := mods/macos/ren-macos.$(SO) mods/firefox/ren-firefox.$(SO) \
	mods/desktop/ren-desktop.$(SO) mods/ren-jarvis/ren-jarvis.$(SO)
WM_MODS-MinGW64 := mods/win32/ren-win32.$(SO) mods/firefox/ren-firefox.$(SO) \
	mods/desktop/ren-desktop.$(SO) mods/ren-jarvis/ren-jarvis.$(SO)
WM_MODS-Msys := $(WM_MODS-MinGW64)
WM_MODS-MingW := $(WM_MODS-MinGW64)
WM_MODS := $(WM_MODS-${uname})
ifeq ($(WM_MODS),)
WM_MODS := mods/firefox/ren-firefox.$(SO) mods/desktop/ren-desktop.$(SO) mods/ren-jarvis/ren-jarvis.$(SO)
endif

JARVIS_BACKEND ?= auto
LINUX_VULKAN_SDK := third_party/1.4.328.1
VULKAN_SDK_URL := https://sdk.lunarg.com/sdk/download/1.4.328.1/linux/vulkansdk-linux-x86_64-1.4.328.1.tar.xz

HAVE_CUDA := $(shell command -v nvcc >/dev/null 2>&1 && (ldconfig -p 2>/dev/null | grep -q libcudart || [ -f /usr/local/cuda/lib64/libcudart.so ]) && echo 1)
HAVE_SYS_VULKAN := $(shell pkg-config --exists vulkan 2>/dev/null && command -v glslc >/dev/null 2>&1 && echo 1)
HAVE_SDK_VULKAN := $(shell [ -f "$(LINUX_VULKAN_SDK)/x86_64/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake" ] && echo 1)
HAVE_OPENBLAS := $(shell pkg-config --exists openblas 2>/dev/null && echo 1)

ifeq ($(JARVIS_BACKEND),auto)
  ifeq (${uname},Darwin)
    JARVIS_BACKEND_EFF := metal
  else ifeq (${uname},Linux)
    ifneq ($(HAVE_CUDA),)
      JARVIS_BACKEND_EFF := cuda
    else ifneq ($(or $(HAVE_SYS_VULKAN),$(HAVE_SDK_VULKAN)),)
      JARVIS_BACKEND_EFF := vulkan
    else
      JARVIS_BACKEND_EFF := cpu
    endif
  else ifeq (${SYS},Windows)
    ifneq ($(HAVE_CUDA),)
      JARVIS_BACKEND_EFF := cuda
    else ifneq ($(VULKAN_SDK),)
      JARVIS_BACKEND_EFF := vulkan
    else
      JARVIS_BACKEND_EFF := cpu
    endif
  else
    JARVIS_BACKEND_EFF := cpu
  endif
  whispercpp := submodules/whisper.cpp/build
else
  JARVIS_BACKEND_EFF := $(JARVIS_BACKEND)
  whispercpp := submodules/whisper.cpp/build-$(JARVIS_BACKEND_EFF)
endif

WHISPER_A := $(whispercpp)/src/libwhisper.a
PARAKEET_A := $(whispercpp)/src/libparakeet.a
JARVISD_BIN := bin/jarvisd$(EXE)

$(LINUX_VULKAN_SDK)/x86_64/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake:
	mkdir -p third_party
	(curl -fsSL "$(VULKAN_SDK_URL)" -o third_party/vulkan-sdk.tar.xz || wget -qO third_party/vulkan-sdk.tar.xz "$(VULKAN_SDK_URL)")
	tar -xf third_party/vulkan-sdk.tar.xz -C third_party

WHISPER_CMAKE_COMMON := -DBUILD_SHARED_LIBS=OFF \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DWHISPER_BUILD_EXAMPLES=OFF \
	-DWHISPER_BUILD_TESTS=OFF \
	-DWHISPER_BUILD_SERVER=OFF

ifeq ($(JARVIS_BACKEND_EFF),cuda)
  WHISPER_CMAKE_BACKEND := -DGGML_CUDA=ON -DGGML_VULKAN=OFF -DGGML_METAL=OFF
else ifeq ($(JARVIS_BACKEND_EFF),vulkan)
  ifeq (${uname},Linux)
    ifneq ($(HAVE_SYS_VULKAN),)
      WHISPER_CMAKE_BACKEND := -DGGML_VULKAN=ON -DGGML_CUDA=OFF
      WHISPER_CMAKE_DEPS :=
    else
      WHISPER_CMAKE_BACKEND := -DCMAKE_PREFIX_PATH=$(CURDIR)/$(LINUX_VULKAN_SDK)/x86_64 -DGGML_VULKAN=ON -DGGML_CUDA=OFF
      WHISPER_CMAKE_DEPS := $(LINUX_VULKAN_SDK)/x86_64/share/cmake/SPIRV-Headers/SPIRV-HeadersConfig.cmake
    endif
  else
    WHISPER_CMAKE_BACKEND := -DGGML_VULKAN=ON -DGGML_CUDA=OFF
  endif
else ifeq ($(JARVIS_BACKEND_EFF),metal)
  WHISPER_CMAKE_BACKEND := -DGGML_METAL=ON -DGGML_OPENMP=OFF
else
  WHISPER_CMAKE_BACKEND := -DGGML_VULKAN=OFF -DGGML_CUDA=OFF -DGGML_METAL=OFF $(if $(HAVE_OPENBLAS),-DGGML_BLAS=ON -DGGML_BLAS_VENDOR=OpenBLAS)
endif

ifeq (${SYS},Windows)
  WHISPER_CMAKE_GEN := -G "MSYS Makefiles" -DGGML_OPENMP=OFF
else
  WHISPER_CMAKE_GEN :=
endif

$(whispercpp)/Makefile: $(WHISPER_CMAKE_DEPS)
	@echo "[INFO] Configuring whisper.cpp ($(JARVIS_BACKEND_EFF)) in $(whispercpp)"
	@mkdir -p $(whispercpp)
	cd $(whispercpp) && cmake .. $(WHISPER_CMAKE_GEN) $(WHISPER_CMAKE_BACKEND) $(WHISPER_CMAKE_COMMON)

all: $(WM_MODS) $(JARVISD_BIN)

mods/sway/ren-sway.$(SO): mods/sway/ren-sway.c
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/sway/ren-sway.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) $(MOD_RPATH_${SYS})

mods/hyprland/ren-hyprland.$(SO): mods/hyprland/ren-hyprland.c
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/hyprland/ren-hyprland.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) $(MOD_RPATH_${SYS})

mods/x11/ren-x11.$(SO): mods/x11/ren-x11.c mods/x11/x11_core.h include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/x11/ren-x11.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -lX11 -ldl $(MOD_RPATH_${SYS})

mods/macos/ren-macos.$(SO): mods/macos/ren-macos.m include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) $(CFLAGS-m-Darwin) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/macos/ren-macos.m $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -framework Cocoa -framework ApplicationServices -framework Carbon -lobjc $(MOD_RPATH_Darwin)

mods/win32/ren-win32.$(SO): mods/win32/ren-win32.c include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -shared -o $@ mods/win32/ren-win32.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -luser32 -lgdi32 -lshell32 -lpsapi

ifeq (${uname},Darwin)
mods/firefox/ren-firefox.$(SO): mods/firefox/ren-firefox.c mods/firefox/firefox_core.h mods/desktop/ax_core.m mods/desktop/ax_core.h mods/desktop/a11y_core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/firefox/ren-firefox.c $(CFLAGS-m-Darwin) mods/desktop/ax_core.m $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -framework ApplicationServices -framework CoreFoundation -framework AppKit -framework Carbon -lobjc -ldl $(MOD_RPATH_Darwin)

mods/desktop/ren-desktop.$(SO): mods/desktop/ren-desktop.c mods/desktop/ax_core.m mods/desktop/ax_core.h mods/desktop/a11y_core.h include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/desktop/ren-desktop.c $(CFLAGS-m-Darwin) mods/desktop/ax_core.m $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -framework ApplicationServices -framework CoreFoundation -framework AppKit -framework Carbon -lobjc -ldl $(MOD_RPATH_Darwin)
else ifeq (${SYS},Windows)
mods/firefox/ren-firefox.$(SO): mods/firefox/ren-firefox.c mods/firefox/firefox_core.h mods/desktop/uia_core.c mods/desktop/uia_core.h mods/desktop/a11y_core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -shared -o $@ mods/firefox/ren-firefox.c mods/desktop/uia_core.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -lole32 -loleaut32 -luuid -luser32 -lpsapi

mods/desktop/ren-desktop.$(SO): mods/desktop/ren-desktop.c mods/desktop/uia_core.c mods/desktop/uia_core.h mods/desktop/a11y_core.h include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -shared -o $@ mods/desktop/ren-desktop.c mods/desktop/uia_core.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -lole32 -loleaut32 -luuid -luser32 -lpsapi
else
mods/firefox/ren-firefox.$(SO): mods/firefox/ren-firefox.c mods/firefox/firefox_core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/firefox/ren-firefox.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) $(if $(filter Unix,$(SYS)),-ldl) $(MOD_RPATH_${SYS})

mods/desktop/ren-desktop.$(SO): mods/desktop/ren-desktop.c mods/desktop/atspi_core.h mods/desktop/a11y_core.h include/ttypt/desktop-core.h
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/desktop/ren-desktop.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) -ldl $(MOD_RPATH_${SYS})
endif

mods/ren-jarvis/ren-jarvis.$(SO): mods/ren-jarvis/ren-jarvis.c
	@mkdir -p mods/ren-jarvis
	$(cc) $(CFLAGS) -I../axil-auren/include -I../libxylem/include -fPIC -shared -o $@ mods/ren-jarvis/ren-jarvis.c $(LDFLAGS) $(MOD_DEPS_LDFLAGS) $(MOD_RPATH_${SYS})

$(WHISPER_A) $(PARAKEET_A): $(whispercpp)/Makefile
	$(MAKE) -C $(whispercpp) -j4 whisper parakeet

GGML_STATIC_LIBS = $(whispercpp)/ggml/src/libggml.a \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-cuda/libggml-cuda.a" ] && echo "$(whispercpp)/ggml/src/ggml-cuda/libggml-cuda.a") \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-vulkan/libggml-vulkan.a" ] && echo "$(whispercpp)/ggml/src/ggml-vulkan/libggml-vulkan.a") \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-metal/libggml-metal.a" ] && echo "$(whispercpp)/ggml/src/ggml-metal/libggml-metal.a") \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-blas/libggml-blas.a" ] && echo "$(whispercpp)/ggml/src/ggml-blas/libggml-blas.a") \
	$(whispercpp)/ggml/src/libggml-cpu.a \
	$(whispercpp)/ggml/src/libggml-base.a

GGML_LINK_EXTRA = \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-cuda/libggml-cuda.a" ] && echo "-lcudart -lcublas -lcublasLt -lcuda") \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-vulkan/libggml-vulkan.a" ] && (if [ "${SYS}" = "Windows" ]; then echo "-L\"$(VULKAN_SDK)/Lib\" -lvulkan-1"; else echo "-lvulkan"; fi)) \
	$(shell [ -f "$(whispercpp)/ggml/src/ggml-blas/libggml-blas.a" ] && (if [ "${uname}" != "Darwin" ]; then echo "-lopenblas"; fi))

JARVISD_LIBS-Linux = $(GGML_LINK_EXTRA) -lstdc++ -lgomp -lpthread -ldl -lm
JARVISD_LIBS-Darwin = -lc++ -framework Accelerate -framework Metal -framework Foundation -framework CoreFoundation -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework ApplicationServices -framework Carbon -lpthread -ldl -lm
JARVISD_LIBS-Windows = $(GGML_LINK_EXTRA) -lstdc++ -lws2_32 -lwinmm -lole32 -luuid -lm
JARVISD_LIBS = $(or $(JARVISD_LIBS-${uname}),$(JARVISD_LIBS-${SYS}),$(GGML_LINK_EXTRA) -lstdc++ -lpthread -ldl -lm)

$(JARVISD_BIN): src/jarvisd.c src/jarvis-dsp.c src/jarvis-dsp.h src/jarvis-vad.c src/jarvis-vad.h src/jarvis-kws.c src/jarvis-kws.h src/jarvis-stt.c src/jarvis-stt.h src/jarvis-stt-backend.h src/jarvis-stt-whisper.c src/jarvis-stt-parakeet.c src/jarvis-stream.c src/jarvis-stream.h src/jarvis-dispatch.c src/jarvis-ipc-srv.c src/jarvis-ipc-srv.h src/jarvis-tts.c src/jarvis-tts.h $(WHISPER_A) $(PARAKEET_A) lib/libjarvis.$(SO)
	@mkdir -p bin
	$(cc) -Iinclude -Isrc -I$(whispercpp)/../include -I$(whispercpp)/../ggml/include $(CFLAGS) -o $@ src/jarvisd.c src/jarvis-dsp.c src/jarvis-vad.c src/jarvis-kws.c src/jarvis-stt.c src/jarvis-stt-whisper.c src/jarvis-stt-parakeet.c src/jarvis-stream.c src/jarvis-dispatch.c src/jarvis-ipc-srv.c src/jarvis-tts.c \
		$(WHISPER_A) $(PARAKEET_A) $(GGML_STATIC_LIBS) \
		-Llib -ljarvis -L../libstele/lib -lstele $(LDFLAGS) $(JARVISD_LIBS) $(or $(LDFLAGS-jarvis-${uname}),$(LDFLAGS-jarvis-${SYS}))

test: all
	$(MAKE) -C tests WHISPER_BUILD=../$(whispercpp) test

${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc: jarvis.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 jarvis.pc $@

${DESTDIR}${PREFIX}/lib/ren/ren-sway.$(SO): mods/sway/ren-sway.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/sway/ren-sway.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-hyprland.$(SO): mods/hyprland/ren-hyprland.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/hyprland/ren-hyprland.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-x11.$(SO): mods/x11/ren-x11.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/x11/ren-x11.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-macos.$(SO): mods/macos/ren-macos.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/macos/ren-macos.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-win32.$(SO): mods/win32/ren-win32.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/win32/ren-win32.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-firefox.$(SO): mods/firefox/ren-firefox.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/firefox/ren-firefox.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-desktop.$(SO): mods/desktop/ren-desktop.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/desktop/ren-desktop.$(SO) $@

${DESTDIR}${PREFIX}/lib/ren/ren-jarvis.$(SO): mods/ren-jarvis/ren-jarvis.$(SO)
	install -d ${DESTDIR}${PREFIX}/lib/ren
	install -m 755 mods/ren-jarvis/ren-jarvis.$(SO) $@

INSTALLED_WM_MODS := $(patsubst %,${DESTDIR}${PREFIX}/lib/ren/%,$(notdir $(WM_MODS)))

install-extra: ${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc $(INSTALLED_WM_MODS)
	@rm -f ${DESTDIR}${PREFIX}/lib/mod_sway.so ${DESTDIR}${PREFIX}/lib/mod_hyprland.so ${DESTDIR}${PREFIX}/lib/mod_tts.so ${DESTDIR}${PREFIX}/lib/ren/mod_jarvis.so
	@rm -f ${DESTDIR}${PREFIX}/lib/jarvis/jarvis-*.so
	@rmdir ${DESTDIR}${PREFIX}/lib/jarvis 2>/dev/null || true
	@install -d ${DESTDIR}${PREFIX}/bin ${DESTDIR}${PREFIX}/share/jarvis ${DESTDIR}${PREFIX}/share/jarvis/models
	@install -m 644 data/ontology.json ${DESTDIR}${PREFIX}/share/jarvis/ontology.json
	@if [ -f submodules/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin ]; then \
		install -m 644 submodules/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin ${DESTDIR}${PREFIX}/share/jarvis/models/ggml-silero-v6.2.0.bin; \
	fi
	@if [ -f data/99-jarvis-aec.conf ]; then \
		install -m 644 data/99-jarvis-aec.conf ${DESTDIR}${PREFIX}/share/jarvis/99-jarvis-aec.conf; \
	fi
	@if [ -z "${DESTDIR}" ]; then \
		mkdir -p "$(HOME)/.config/jarvis"; \
		install -m 644 data/ontology.json "$(HOME)/.config/jarvis/ontology.json"; \
	fi
	@if [ -f scripts/edge-tts-pcm ]; then \
		install -m 755 scripts/edge-tts-pcm ${DESTDIR}${PREFIX}/bin/edge-tts-pcm; \
	fi
	@if [ -f $(JARVISD_BIN) ]; then \
		install -m 755 $(JARVISD_BIN) ${DESTDIR}${PREFIX}/bin/jarvisd$(EXE); \
		if [ "${SYS}" != "Windows" ]; then ln -sf jarvisd ${DESTDIR}${PREFIX}/bin/dwm-whisperd; fi; \
	fi

install: install-extra install-services

.PHONY: install-services
install-services:
	@if [ "${uname}" = "Linux" ] || [ -d /run/systemd/system ]; then \
		echo "[INFO] Installing systemd services for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/lib/systemd/user; \
		UNIT_PREFIX="$$(echo "${PREFIX}" | sed "s|^$(HOME)|%h|")"; \
		sed "s|@PREFIX@|$${UNIT_PREFIX}|g" services/systemd/jarvisd.service > ${DESTDIR}${PREFIX}/lib/systemd/user/jarvisd.service; \
		if [ -z "${DESTDIR}" ] && { [ -d "$(HOME)/.config/systemd/user" ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; }; then \
			mkdir -p "$(HOME)/.config/systemd/user"; \
			ln -sf "${PREFIX}/lib/systemd/user/jarvisd.service" "$(HOME)/.config/systemd/user/jarvisd.service"; \
		fi; \
	fi
	@if [ -d /etc/init.d ] || [ -x /sbin/openrc-run ] || [ "${distro}" = "alpine" ] || [ "${distro}" = "gentoo" ]; then \
		echo "[INFO] Installing OpenRC service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/examples/openrc; \
		sed 's|@PREFIX@|${PREFIX}|g' services/openrc/jarvisd > ${DESTDIR}${PREFIX}/share/examples/openrc/jarvisd; \
		chmod 755 ${DESTDIR}${PREFIX}/share/examples/openrc/jarvisd; \
		if [ -z "${DESTDIR}" ] && [ -w /etc/init.d ]; then \
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
		if [ -z "${DESTDIR}" ] && [ -w /etc/rc.d ]; then \
			install -m 755 ${DESTDIR}${PREFIX}/share/examples/rc.d/jarvisd /etc/rc.d/jarvisd; \
		fi; \
	elif [ "${uname}" = "Darwin" ]; then \
		echo "[INFO] Installing launchd service for jarvisd..."; \
		install -d ${DESTDIR}${PREFIX}/share/launchd; \
		sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.jarvisd.plist > ${DESTDIR}${PREFIX}/share/launchd/pt.tty.jarvisd.plist; \
		if [ -z "${DESTDIR}" ] && [ -d "$(HOME)/Library/LaunchAgents" ]; then \
			sed 's|@PREFIX@|${PREFIX}|g' services/launchd/pt.tty.jarvisd.plist > "$(HOME)/Library/LaunchAgents/pt.tty.jarvisd.plist"; \
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
		if [ -z "${DESTDIR}" ] && { [ -d "$(HOME)/.config/autostart" ] || [ -n "$$(echo "${PREFIX}" | grep -E '^($(HOME)|~)')" ]; }; then \
			mkdir -p "$(HOME)/.config/autostart"; \
			sed 's|@PREFIX@|${PREFIX}|g' services/xdg/jarvisd.desktop > "$(HOME)/.config/autostart/jarvisd.desktop"; \
		fi; \
	fi

clean: clean-tests
clean-tests:
	rm -f $(WM_MODS) $(JARVISD_BIN) mods/*/*.so mods/*/*.dll src/*.o src/*.o.d
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
