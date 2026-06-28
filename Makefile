# MVCI J2534 driver — Linux build (termios + OpenSSL).
#
#   make            -> lib/libMVCI.so and mvci_test
#   make test       -> run the codec self-test (no hardware)
#   sudo make install
#
# Windows builds use the Visual Studio solution (MVCI.sln) instead.
#
# Layout:  include/mvci/  public headers   (#include <mvci/...>)
#          src/           sources + private headers
#          lib/           built library

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -fPIC -std=c11 -Wno-deprecated-declarations \
           -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Iinclude
LDLIBS   = -lcrypto -lpthread

LIB      = lib/libMVCI.so
LIB_SRCS = src/passthru.c src/serial.c src/io.c src/des.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# the test harness links the protocol layer directly (not the J2534 exports)
TEST_SRCS = test/mvci_test.c src/serial.c src/io.c src/des.c

.PHONY: all test clean install
all: $(LIB) mvci_test

$(LIB): $(LIB_OBJS)
	@mkdir -p lib
	$(CC) -shared -o $@ $^ $(LDLIBS)

mvci_test: $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: mvci_test
	./mvci_test

install: $(LIB)
	install -d /usr/local/lib /usr/local/include/mvci
	install -m 755 $(LIB) /usr/local/lib/
	install -m 644 include/mvci/*.h /usr/local/include/mvci/
	ldconfig

clean:
	rm -f $(LIB_OBJS) $(LIB) mvci_test
