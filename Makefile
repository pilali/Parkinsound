#!/usr/bin/make -f
# Top-level Makefile for the Parkinsound LV2 plugin bundle.
# Designed to work both natively (Linux) and with mod-plugin-builder
# (which provides cross-compilation via CC/CFLAGS/LDFLAGS env vars).

BUNDLE  = parkinsound-stepgate.lv2
TARGET  = $(BUNDLE)/stepgate.so
SOURCES = $(BUNDLE)/stepgate.c

CC ?= gcc

# Allow mod-plugin-builder / Buildroot to inject its own flags; append ours.
override CFLAGS  += -O2 -Wall -Wextra -fPIC -DPIC -fvisibility=hidden
override LDFLAGS += -shared

# mod-plugin-builder honours INSTALL_PATH (defaults to /usr/lib/lv2 on the
# target rootfs). DESTDIR is also respected for staged installs.
INSTALL_PATH ?= /usr/lib/lv2
DESTDIR      ?=

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) -lm

clean:
	rm -f $(TARGET)

install: all
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)
	install -m 644 $(BUNDLE)/manifest.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 644 $(BUNDLE)/stepgate.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 755 $(BUNDLE)/stepgate.so  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
