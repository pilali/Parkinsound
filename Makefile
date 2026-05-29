#!/usr/bin/make -f
# Top-level Makefile for the Parkinsound LV2 plugin bundles.
# Designed to work both natively (Linux) and with mod-plugin-builder
# (which provides cross-compilation via CC/CFLAGS/LDFLAGS env vars).
#
# Two plug-ins live here:
#   - parkinsound-stepgate.lv2  : the original single (stereo) step gate
#   - parkinsound-stepgate4.lv2 : the 4-channel sample-locked sibling
#
# Each has its own build / install targets so the two mod-plugin-builder
# recipes stay independent. `make` builds both.

# ---- Single-channel (stereo) bundle ---------------------------------
BUNDLE   = parkinsound-stepgate.lv2
TARGET   = $(BUNDLE)/stepgate.so
SOURCES  = $(BUNDLE)/stepgate.c

# Factory preset bundles. Add one .ttl per preset following the
# DragonflyPlate convention (filename == preset URI). Both the file
# and a matching <Filename.ttl> stub in manifest.ttl are required.
PRESETS = \
	Off.ttl \
	Four_on_the_Floor.ttl \
	Eighth_Notes.ttl \
	Sixteenth_Chop.ttl

# ---- 4-channel bundle -----------------------------------------------
BUNDLE4  = parkinsound-stepgate4.lv2
TARGET4  = $(BUNDLE4)/stepgate4.so
SOURCES4 = $(BUNDLE4)/stepgate4.c

PRESETS4 = \
	Off.ttl \
	Polyrhythm.ttl

CC ?= gcc

# Allow mod-plugin-builder / Buildroot to inject its own flags; append ours.
override CFLAGS  += -O2 -Wall -Wextra -fPIC -DPIC -fvisibility=hidden
override LDFLAGS += -shared

# mod-plugin-builder honours INSTALL_PATH (defaults to /usr/lib/lv2 on the
# target rootfs). DESTDIR is also respected for staged installs.
INSTALL_PATH ?= /usr/lib/lv2
DESTDIR      ?=

.PHONY: all stepgate stepgate4 clean \
        install install-stepgate install-stepgate4 install-all \
        bump-recipe bump-recipe4

RECIPE_MK  = plugins/package/parkinsound-stepgate/parkinsound-stepgate.mk
RECIPE_MK4 = plugins/package/parkinsound-stepgate4/parkinsound-stepgate4.mk

all: stepgate stepgate4

stepgate:  $(TARGET)
stepgate4: $(TARGET4)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) -lm

$(TARGET4): $(SOURCES4)
	$(CC) $(CFLAGS) -o $@ $(SOURCES4) $(LDFLAGS) -lm

clean:
	rm -f $(TARGET) $(TARGET4)

# ---- Install --------------------------------------------------------
# `install` keeps installing only the single-channel bundle so the
# existing mod-plugin-builder recipe behaves unchanged.
install: install-stepgate

install-all: install-stepgate install-stepgate4

install-stepgate: stepgate
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)
	install -m 644 $(BUNDLE)/manifest.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 644 $(BUNDLE)/stepgate.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 755 $(BUNDLE)/stepgate.so  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	for p in $(PRESETS); do \
		install -m 644 $(BUNDLE)/$$p $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/ ; \
	done
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui
	install -m 644 $(BUNDLE)/modgui/icon-parkinsound-stepgate.html        $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/stylesheet-parkinsound-stepgate.css   $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/javascript-parkinsound-stepgate.js    $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/screenshot-parkinsound-stepgate.png   $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/thumbnail-parkinsound-stepgate.png    $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/

install-stepgate4: stepgate4
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)
	install -m 644 $(BUNDLE4)/manifest.ttl  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/
	install -m 644 $(BUNDLE4)/stepgate4.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/
	install -m 755 $(BUNDLE4)/stepgate4.so  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/
	for p in $(PRESETS4); do \
		install -m 644 $(BUNDLE4)/$$p $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/ ; \
	done
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui
	install -m 644 $(BUNDLE4)/modgui/icon-parkinsound-stepgate4.html      $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui/
	install -m 644 $(BUNDLE4)/modgui/stylesheet-parkinsound-stepgate4.css $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui/
	install -m 644 $(BUNDLE4)/modgui/javascript-parkinsound-stepgate4.js  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui/
	install -m 644 $(BUNDLE4)/modgui/screenshot-parkinsound-stepgate4.png $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui/
	install -m 644 $(BUNDLE4)/modgui/thumbnail-parkinsound-stepgate4.png  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE4)/modgui/

# ---- Recipe version bumping -----------------------------------------
# Rewrites the *_VERSION in a mod-plugin-builder recipe to the current
# git HEAD. Run AFTER committing your source changes, then commit the
# recipe bump as its own follow-up commit.
bump-recipe:
	@hash=$$(git rev-parse HEAD); \
	if [ -z "$$hash" ]; then echo "error: not a git repository"; exit 1; fi; \
	sed -i.bak "s|^PARKINSOUND_STEPGATE_VERSION = .*|PARKINSOUND_STEPGATE_VERSION = $$hash|" $(RECIPE_MK); \
	rm -f $(RECIPE_MK).bak; \
	echo "Recipe bumped to $$hash"; \
	echo "Stage with: git add $(RECIPE_MK)"

bump-recipe4:
	@hash=$$(git rev-parse HEAD); \
	if [ -z "$$hash" ]; then echo "error: not a git repository"; exit 1; fi; \
	sed -i.bak "s|^PARKINSOUND_STEPGATE4_VERSION = .*|PARKINSOUND_STEPGATE4_VERSION = $$hash|" $(RECIPE_MK4); \
	rm -f $(RECIPE_MK4).bak; \
	echo "Recipe bumped to $$hash"; \
	echo "Stage with: git add $(RECIPE_MK4)"
