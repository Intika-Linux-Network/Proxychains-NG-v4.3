#
# Makefile for proxybound
#
# Use config.mak to override any of the following variables.
# Do not make changes here.
#

exec_prefix = /usr/local
bindir = $(exec_prefix)/bin

prefix = /usr/local/
includedir = $(prefix)/include
libdir = $(prefix)/lib
sysconfdir=$(prefix)/etc

SRCS = $(sort $(wildcard src/*.c))
OBJS = $(SRCS:.c=.o)
LOBJS = src/core.o src/common.o src/libproxybound.o src/hostsreader.o src/ip-type.o 

CFLAGS  += -Wall -O0 -g -std=c99 -D_GNU_SOURCE -pipe
LDFLAGS = -shared -fPIC -Wl,--no-as-needed -ldl -lpthread
INC     = 
PIC     = -fPIC
AR      = $(CROSS_COMPILE)ar
RANLIB  = $(CROSS_COMPILE)ranlib

LDSO_SUFFIX = so
LD_SET_SONAME = -Wl,-soname=
INSTALL_FLAGS = -D -m

-include config.mak

LDSO_PATHNAME = libproxybound.$(LDSO_SUFFIX)

SHARED_LIBS = $(LDSO_PATHNAME)
ALL_LIBS = $(SHARED_LIBS)
PXCHAINS = proxybound
ALL_TOOLS = $(PXCHAINS)


CFLAGS+=$(USER_CFLAGS) $(MAC_CFLAGS)
CFLAGS_MAIN=-DLIB_DIR=\"$(libdir)\" -DSYSCONFDIR=\"$(sysconfdir)\" -DDLL_NAME=\"$(LDSO_PATHNAME)\"


all: $(ALL_LIBS) $(ALL_TOOLS)

debug: CFLAGS += -D DEBUG
debug: $(ALL_LIBS) $(ALL_TOOLS)

install-config:
	install -d $(DESTDIR)/$(sysconfdir)
	install $(INSTALL_FLAGS) 644 src/proxybound.conf $(DESTDIR)/$(sysconfdir)/

install: 
	install -d $(DESTDIR)/$(bindir)/ $(DESTDIR)/$(libdir)/
	install $(INSTALL_FLAGS) 755 $(ALL_TOOLS) $(DESTDIR)/$(bindir)/
	install $(INSTALL_FLAGS) 644 $(ALL_LIBS) $(DESTDIR)/$(libdir)/

clean:
	rm -f $(ALL_LIBS)
	rm -f $(ALL_TOOLS)
	rm -f $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $(CFLAGS_MAIN) $(INC) $(PIC) -c -o $@ $<

$(LDSO_PATHNAME): $(LOBJS)
	$(CC) $(LDFLAGS) $(LD_SET_SONAME)$(LDSO_PATHNAME) -o $@ $(LOBJS)

$(ALL_TOOLS): $(OBJS)
	$(CC) src/main.o src/common.o -o $(PXCHAINS)


.PHONY: all clean install
