include ../../mk/portable.mk

CC ?= $(cc)
CFLAGS := -g -O2 -Wall -Wextra -Wpedantic -fPIC -I../include -I../../libstele/include -I../../axil-auren/include -I../../libxylem/include
LDFLAGS := -L../lib -L../../libstele/lib -L../../axil-auren/lib -L../../libxylem/lib
ifeq (${SYS},Unix)
LDFLAGS += -rdynamic -Wl,-rpath,${CURDIR}/../lib -Wl,-rpath,${CURDIR}/../../libstele/lib -Wl,-rpath,${CURDIR}/../../axil-auren/lib -Wl,-rpath,${CURDIR}/../../libxylem/lib
endif

all: test_jarvis test_ren_mods test_tts test_seams

TEST_SYS_LIBS :=
ifeq (${SYS},Unix)
TEST_SYS_LIBS := -lpthread -ldl
else ifeq (${SYS},Windows)
TEST_SYS_LIBS := -lws2_32
endif

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

test: all
	LD_LIBRARY_PATH=../lib:../../libstele/lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_seams
	LD_LIBRARY_PATH=../lib:../../libstele/lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_jarvis
	LD_LIBRARY_PATH=../lib:../../libstele/lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_ren_mods
	LD_LIBRARY_PATH=../lib:../../libstele/lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_tts
	../bin/jarvis --help >/dev/null 2>&1 || [ $$? -eq 0 ]
	../bin/jarvis --dry-run --json "open a terminal" | grep -q '"dry_run":true'
	@tmp_rc=$$(mktemp); \
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
	rm -f test_jarvis test_ren_mods test_tts test_seams mock_adapter.so *.o
