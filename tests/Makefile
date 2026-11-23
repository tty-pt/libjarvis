CC ?= cc
CFLAGS := -g -O2 -Wall -Wextra -Wpedantic -fPIC -I../include -I../../axil-auren/include -I../../libxylem/include
LDFLAGS := -rdynamic -L../lib -L../../axil-auren/lib -L../../libxylem/lib \
           -Wl,-rpath,${CURDIR}/../lib -Wl,-rpath,${CURDIR}/../../axil-auren/lib -Wl,-rpath,${CURDIR}/../../libxylem/lib

all: test_jarvis test_ren_mods test_tts

test_jarvis: test_jarvis.c ../lib/libjarvis.so
	$(CC) $(CFLAGS) -o $@ test_jarvis.c -L../lib -Wl,-rpath,${CURDIR}/../lib -ljarvis -lm

test_ren_mods: test_ren_mods.c ../../axil-auren/lib/libren.so
	$(CC) $(CFLAGS) -o $@ test_ren_mods.c $(LDFLAGS) -lren -lxylem -ldl -lpthread

test_tts: test_tts.c ../src/jarvis-tts.c ../src/jarvis-tts.h
	$(CC) $(CFLAGS) -I../src -o $@ test_tts.c ../src/jarvis-tts.c -lpthread -ldl -lm

test: all
	LD_LIBRARY_PATH=../lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_jarvis
	LD_LIBRARY_PATH=../lib:../../axil-auren/lib:../../libxylem/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_ren_mods
	./test_tts
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
	rm -f test_jarvis test_ren_mods test_tts mock_adapter.so *.o
