# GWM (Geo's Window Manager) — infinite-canvas compositing WM
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

ARCH    := $(shell $(CC) -print-multiarch 2>/dev/null)

# Use real headers when dev packages exist, else the vendored minimal ones.
HAVE_XCOMP_H := $(shell test -f /usr/include/X11/extensions/Xcomposite.h && echo 1 || echo 0)
HAVE_DAMAGE  := $(shell test -f /usr/include/X11/extensions/Xdamage.h && echo 1 || echo 0)
HAVE_XTEST_H := $(shell test -f /usr/include/X11/extensions/XTest.h && echo 1 || echo 0)
HAVE_IMLIB2  := $(shell test -f /usr/include/Imlib2.h && echo 1 || echo 0)
HAVE_XINERAMA := $(shell test -f /usr/include/X11/extensions/Xinerama.h && echo 1 || echo 0)

INC :=
ifeq ($(HAVE_XCOMP_H),0)
INC += -Ivendor
endif
ifeq ($(HAVE_XTEST_H),0)
INC += -Ivendor
endif

# Link helper: prefer -lFoo, fall back to the bare .so.N runtime object.
define findlib
$(shell if [ -e /usr/lib/$(ARCH)/lib$(1).so ] || [ -e /usr/lib/lib$(1).so ]; then \
  echo -l$(1); \
else \
  ls /usr/lib/$(ARCH)/lib$(1).so.* /lib/$(ARCH)/lib$(1).so.* 2>/dev/null | head -1; \
fi)
endef

LIB_XCOMP  := $(call findlib,Xcomposite)
LIB_XREND  := $(call findlib,Xrender)
LIB_XTST   := $(call findlib,Xtst)
LIB_XDAM   := $(if $(filter 1,$(HAVE_DAMAGE)),$(call findlib,Xdamage),)
LIB_IMLIB  := $(if $(filter 1,$(HAVE_IMLIB2)),$(call findlib,Imlib2),)
LIB_XIN    := $(if $(filter 1,$(HAVE_XINERAMA)),$(call findlib,Xinerama),)

all: gwm

gwm: src/gwm.c
	$(CC) $(CFLAGS) $(INC) -DHAVE_DAMAGE=$(HAVE_DAMAGE) \
	  -DHAVE_IMLIB2=$(HAVE_IMLIB2) -DHAVE_XINERAMA=$(HAVE_XINERAMA) -o $@ $< \
	  -lX11 $(LIB_XREND) $(LIB_XCOMP) $(LIB_XDAM) $(LIB_IMLIB) $(LIB_XIN) -lm

tools: xdo demoapp

xdo: tools/xdo.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lX11 $(LIB_XTST)

demoapp: tools/demoapp.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

install: gwm
	install -D -m755 gwm $(DESTDIR)$(PREFIX)/bin/gwm

clean:
	rm -f gwm xdo demoapp

.PHONY: all tools install clean
