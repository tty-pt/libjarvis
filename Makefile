INSTALL_BIN := jarvis
all := libjarvis jarvis

libjarvis-obj-y := src/jarvis-json.o src/jarvis-ontology.o src/jarvis-solver.o src/jarvis-voice.o src/jarvis-adapters.o
jarvis-obj-y := src/jarvis-cli-adapter.o
LDLIBS-libjarvis := -lxylem -lstele -lcorm -lm
LDLIBS-jarvis := -Llib -ljarvis -lxylem -lstele -lcorm -lm
LDFLAGS-jarvis-Linux := -Wl,-rpath,${CURDIR}/lib
CFLAGS-BIN-Windows :=

CFLAGS := -g -O2 -Iinclude -I../libxylem/include -I../libstele/include -I../libcorm/include
LDFLAGS := -L../libxylem/lib -L../libstele/lib -L../libcorm/lib

WASM_SRCS := npm/wasm/jarvis_wasm.c
WASM_EXTRA_CFLAGS := -Iinclude -I../libxylem/include -I../libstele/include
WASM_GLOBAL_BASE := 4194304
WASM_LDFLAGS := -Wl,-z,stack-size=524288

include ../mk/include.mk

test: all
	$(MAKE) -C tests test

${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc: jarvis.pc
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 jarvis.pc $@

install: ${DESTDIR}${PREFIX}/lib/pkgconfig/jarvis.pc

clean: clean-tests
clean-tests:
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
