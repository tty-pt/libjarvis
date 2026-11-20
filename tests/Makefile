CC ?= cc
CFLAGS := -g -O2 -Wall -Wextra -fPIC -I../include -I../../libmm/include -I../../libxylem/include -I../../libstele/include -I../../libcorm/include
LDFLAGS := -rdynamic -L. -L../lib -L../../libmm/lib -L../../libxylem/lib -L../../libstele/lib -L../../libcorm/lib \
           -Wl,-rpath,${CURDIR} -Wl,-rpath,${CURDIR}/../lib -Wl,-rpath,${CURDIR}/../../libmm/lib -Wl,-rpath,${CURDIR}/../../libxylem/lib \
           -Wl,-rpath,${CURDIR}/../../libstele/lib -Wl,-rpath,${CURDIR}/../../libcorm/lib
LDLIBS := -ljarvis -lmm -lxylem -lstele -lcorm -lm

all: mock_adapter.so test_jarvis

mock_adapter.so: mock_adapter.c ../lib/libjarvis.so
	$(CC) $(CFLAGS) -shared -o $@ mock_adapter.c $(LDFLAGS) $(LDLIBS)

test_jarvis: test_jarvis.c mock_adapter.so ../lib/libjarvis.so
	$(CC) $(CFLAGS) -o $@ test_jarvis.c ./mock_adapter.so $(LDFLAGS) $(LDLIBS)

test: all
	LD_LIBRARY_PATH=../lib:../../libmm/lib:../../libxylem/lib:../../libstele/lib:../../libcorm/lib:${HOME}/.local/lib:$${LD_LIBRARY_PATH:-} ./test_jarvis

clean:
	rm -f test_jarvis mock_adapter.so *.o
