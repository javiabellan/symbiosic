CC ?= cc
PKG_CONFIG ?= pkg-config
# llhttp does not ship a .pc file on all systems (including Homebrew).
PREFIX ?= $(shell test -d /opt/homebrew && echo /opt/homebrew || echo /usr/local)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -I$(PREFIX)/include $(shell $(PKG_CONFIG) --cflags libcurl libuv yyjson 2>/dev/null) $(shell $(PKG_CONFIG) --cflags libllhttp 2>/dev/null)
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra
LDFLAGS += -L$(PREFIX)/lib
BASE_LIBS := $(shell $(PKG_CONFIG) --libs libcurl libuv yyjson 2>/dev/null || echo '-lcurl -luv -lyyjson')
LDLIBS += $(BASE_LIBS) $(shell $(PKG_CONFIG) --libs libllhttp 2>/dev/null || echo '-lllhttp') $(if $(filter -lm,$(BASE_LIBS)),,-lm)
TIKTOKEN_DIR ?= .deps
CPPFLAGS += -I$(TIKTOKEN_DIR)
LDLIBS += $(TIKTOKEN_DIR)/libtiktoken_c.a $(if $(filter Darwin,$(shell uname -s)),-liconv,-ldl -lpthread)
SOURCES = fidelity.c providers.c util.c store.c normalize.c query.c ai.c chat.c main.c http.c
OBJECTS = $(SOURCES:.c=.o)
.PHONY: all test clean sanitize deps
all: traceview
deps:
	sh scripts/install-tiktoken.sh $(TIKTOKEN_DIR)
traceview: $(OBJECTS) $(TIKTOKEN_DIR)/libtiktoken_c.a
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)
%.o: %.c app.h $(TIKTOKEN_DIR)/tiktoken.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
tests/test_traceview: tests/test.c $(SOURCES) app.h $(TIKTOKEN_DIR)/libtiktoken_c.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTRACEVIEW_TEST -I. $(LDFLAGS) -o $@ tests/test.c $(SOURCES) $(LDLIBS)
test: tests/test_traceview traceview
	./tests/test_traceview
	python3 tests/http_test.py
sanitize: $(TIKTOKEN_DIR)/libtiktoken_c.a
	$(CC) $(CPPFLAGS) -std=c17 -Wall -Wextra -g -O1 -fsanitize=address,undefined -DTRACEVIEW_TEST -I. $(LDFLAGS) -o tests/test_sanitize tests/test.c $(SOURCES) $(LDLIBS)
	./tests/test_sanitize
clean:
	rm -f traceview $(OBJECTS) tests/test_traceview tests/test_sanitize
