include ../../mk/portable.mk

CC ?= $(cc)
CFLAGS := -g -O2 -Wall -Wextra -Wpedantic -fPIC -I../include -I../../libstele/include -I../../axil-auren/include -I../../libxylem/include
LDFLAGS := -L../lib -L../../libstele/lib -L../../axil-auren/lib -L../../libxylem/lib
ifeq (${SYS},Unix)
LDFLAGS += -rdynamic -Wl,-rpath,${CURDIR}/../lib -Wl,-rpath,${CURDIR}/../../libstele/lib -Wl,-rpath,${CURDIR}/../../axil-auren/lib -Wl,-rpath,${CURDIR}/../../libxylem/lib
endif

all: test_jarvis test_ren_mods test_tts test_seams test_dsp test_vad test_stt_backend test_kws test_stream

TEST_SYS_LIBS :=
ifeq (${SYS},Unix)
TEST_SYS_LIBS := -lpthread -ldl
else ifeq (${SYS},Windows)
TEST_SYS_LIBS := -lws2_32
endif

WHISPER_BUILD ?= ../submodules/whisper.cpp/build
WHISPER_A := $(WHISPER_BUILD)/src/libwhisper.a
PARAKEET_A := $(WHISPER_BUILD)/src/libparakeet.a
GGML_TEST_LIBS = $(WHISPER_BUILD)/ggml/src/libggml.a \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-cuda/libggml-cuda.a" ] && echo "$(WHISPER_BUILD)/ggml/src/ggml-cuda/libggml-cuda.a") \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-vulkan/libggml-vulkan.a" ] && echo "$(WHISPER_BUILD)/ggml/src/ggml-vulkan/libggml-vulkan.a") \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-metal/libggml-metal.a" ] && echo "$(WHISPER_BUILD)/ggml/src/ggml-metal/libggml-metal.a") \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-blas/libggml-blas.a" ] && echo "$(WHISPER_BUILD)/ggml/src/ggml-blas/libggml-blas.a") \
	$(WHISPER_BUILD)/ggml/src/libggml-cpu.a \
	$(WHISPER_BUILD)/ggml/src/libggml-base.a
GGML_TEST_EXTRA = \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-cuda/libggml-cuda.a" ] && echo "-lcudart -lcublas -lcublasLt -lcuda") \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-vulkan/libggml-vulkan.a" ] && (if [ "${SYS}" = "Windows" ]; then echo "-L\"$(VULKAN_SDK)/Lib\" -lvulkan-1"; else echo "-lvulkan"; fi)) \
	$(shell [ -f "$(WHISPER_BUILD)/ggml/src/ggml-blas/libggml-blas.a" ] && (if [ "${uname}" != "Darwin" ]; then echo "-lopenblas"; fi))
VAD_EXTRA_LIBS-Linux = $(GGML_TEST_EXTRA) -lstdc++ -lgomp -lpthread -ldl -lm
VAD_EXTRA_LIBS-Darwin = -lc++ -framework Accelerate -framework Metal -framework Foundation -lpthread -ldl -lm
VAD_EXTRA_LIBS = $(or $(VAD_EXTRA_LIBS-${uname}),$(GGML_TEST_EXTRA) -lstdc++ $(TEST_SYS_LIBS) -lm)

test_jarvis: test_jarvis.c ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -o $@ test_jarvis.c $(LDFLAGS) -ljarvis -lstele -lm $(if $(filter Windows,$(SYS)),-lws2_32)

ifeq (${uname},Darwin)
test_ren_mods: test_ren_mods.c ../../axil-auren/lib/libren.$(SO) ../mods/desktop/ax_core.m
	$(CC) $(CFLAGS) -o $@ test_ren_mods.c $(CFLAGS-m-Darwin) ../mods/desktop/ax_core.m $(LDFLAGS) -lren -lxylem -lstele -framework ApplicationServices -framework CoreFoundation -framework AppKit -framework Carbon -lobjc -ldl -lpthread
else ifeq (${SYS},Windows)
test_ren_mods: test_ren_mods.c ../../axil-auren/lib/libren.$(SO) ../mods/desktop/uia_core.c
	$(CC) $(CFLAGS) -o $@ test_ren_mods.c ../mods/desktop/uia_core.c $(LDFLAGS) -lren -lxylem -lstele -lole32 -loleaut32 -luuid -luser32 -lpsapi
else
test_ren_mods: test_ren_mods.c ../../axil-auren/lib/libren.$(SO)
	$(CC) $(CFLAGS) -o $@ test_ren_mods.c $(LDFLAGS) -lren -lxylem -lstele -ldl -lpthread
endif

test_tts: test_tts.c ../src/jarvis-tts.c ../src/jarvis-tts.h ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -I../src -o $@ test_tts.c ../src/jarvis-tts.c $(LDFLAGS) -ljarvis -lstele $(TEST_SYS_LIBS) -lm

test_seams: test_seams.c ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -I../src -o $@ test_seams.c $(LDFLAGS) -ljarvis -lstele $(if $(filter Unix,$(SYS)),-lpthread) $(if $(filter Windows,$(SYS)),-lws2_32) -lm

test_dsp: test_dsp.c ../src/jarvis-dsp.c ../src/jarvis-dsp.h ../src/jarvis-stt.c ../src/jarvis-stt.h
	$(CC) $(CFLAGS) -I../src -I../submodules/whisper.cpp/include -I../submodules/whisper.cpp/ggml/include -o $@ test_dsp.c ../src/jarvis-dsp.c ../src/jarvis-stt.c $(if $(filter Unix,$(SYS)),-lpthread) -lm

test_vad: test_vad.c ../src/jarvis-vad.c ../src/jarvis-vad.h $(WHISPER_A) ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -I../src -I../submodules/whisper.cpp/include -I../submodules/whisper.cpp/ggml/include -o $@ test_vad.c ../src/jarvis-vad.c $(WHISPER_A) $(GGML_TEST_LIBS) $(LDFLAGS) -ljarvis -lstele $(VAD_EXTRA_LIBS)

test_stt_backend: test_stt_backend.c ../src/jarvis-stt-backend.h ../src/jarvis-stt-whisper.c ../src/jarvis-stt-parakeet.c ../src/jarvis-stt.c ../src/jarvis-stt.h ../src/jarvis-stream.c ../src/jarvis-stream.h ../src/jarvis-ipc-srv.c ../src/jarvis-ipc-srv.h $(WHISPER_A) $(PARAKEET_A) ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -I../src -I../submodules/whisper.cpp/include -I../submodules/whisper.cpp/ggml/include -o $@ test_stt_backend.c ../src/jarvis-stt-whisper.c ../src/jarvis-stt-parakeet.c ../src/jarvis-stt.c ../src/jarvis-stream.c ../src/jarvis-ipc-srv.c $(WHISPER_A) $(PARAKEET_A) $(GGML_TEST_LIBS) $(LDFLAGS) -ljarvis -lstele $(VAD_EXTRA_LIBS)

test_kws: test_kws.c ../src/jarvis-kws.c ../src/jarvis-kws.h ../lib/libjarvis.$(SO)
	$(CC) $(CFLAGS) -I../src -o $@ test_kws.c ../src/jarvis-kws.c $(LDFLAGS) -ljarvis -lstele $(TEST_SYS_LIBS) -lm

test_stream: test_stream.c ../src/jarvis-stream.c ../src/jarvis-stream.h ../src/jarvis-stt-backend.h
	$(CC) $(CFLAGS) -I../src -o $@ test_stream.c ../src/jarvis-stream.c -lm

TEST_LD_PATH := ../lib:../../libstele/lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib

test: all
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_dsp
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_vad
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_stt_backend
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_kws
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_stream
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_seams
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_jarvis
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_ren_mods
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ./test_tts
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ../bin/jarvis --help >/dev/null 2>&1 || [ $$? -eq 0 ]
	LD_LIBRARY_PATH=$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-} ../bin/jarvis --dry-run --json "open a terminal" | grep -q '"dry_run":true'
	@tmp_rc=$$(mktemp); \
	export LD_LIBRARY_PATH="$(TEST_LD_PATH):$${LD_LIBRARY_PATH:-}"; \
	JARVISRC="$$tmp_rc" ../bin/jarvis config wake_word Jarbas; \
	val=$$(JARVISRC="$$tmp_rc" ../bin/jarvis config wake_word); \
	JARVISRC="$$tmp_rc" ../bin/jarvis config tts_lang en-us; \
	JARVISRC="$$tmp_rc" ../bin/jarvis config lang pt; \
	lval=$$(JARVISRC="$$tmp_rc" ../bin/jarvis config lang); \
	stale=$$(JARVISRC="$$tmp_rc" ../bin/jarvis config tts_lang || true); \
	JARVISRC="$$tmp_rc" ../bin/jarvis doctor --json | grep -q '"config":'; \
	JARVISRC="$$tmp_rc" ../bin/jarvis setup --dry-run --json | grep -q '"dry_run":true'; \
	rm -f "$$tmp_rc"; \
	[ "$$val" = "Jarbas" ] || (echo "CLI config test failed: got '$$val'"; exit 1); \
	[ "$$lval" = "pt" ] && [ -z "$$stale" ] || (echo "CLI one-line lang test failed: lang='$$lval' stale='$$stale'"; exit 1)
	@echo "[PASS] bin/jarvis CLI smoke tests passed!"

clean:
	rm -f test_jarvis test_ren_mods test_tts test_seams test_dsp test_vad test_stt_backend test_kws test_stream mock_adapter.so *.o
