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

.PHONY: all clean install bump-recipe

RECIPE_MK = plugins/package/parkinsound-stepgate/parkinsound-stepgate.mk

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) $(LDFLAGS) -lm

clean:
	rm -f $(TARGET)

install: all
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)
	install -m 644 $(BUNDLE)/manifest.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 644 $(BUNDLE)/stepgate.ttl $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 644 $(BUNDLE)/presets.ttl  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -m 755 $(BUNDLE)/stepgate.so  $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/
	install -d $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui
	install -m 644 $(BUNDLE)/modgui/icon-parkinsound-stepgate.html        $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/stylesheet-parkinsound-stepgate.css   $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/javascript-parkinsound-stepgate.js    $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/screenshot-parkinsound-stepgate.png   $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/
	install -m 644 $(BUNDLE)/modgui/thumbnail-parkinsound-stepgate.png    $(DESTDIR)$(INSTALL_PATH)/$(BUNDLE)/modgui/

# Rewrites PARKINSOUND_STEPGATE_VERSION in the mod-plugin-builder recipe
# to point at the current git HEAD. Run AFTER committing your source
# changes, then commit the recipe bump as its own follow-up commit:
#
#   git commit -m "Source changes"
#   make bump-recipe
#   git add $(RECIPE_MK) && git commit -m "Bump recipe to <hash>"
bump-recipe:
	@hash=$$(git rev-parse HEAD); \
	if [ -z "$$hash" ]; then \
		echo "error: not a git repository"; exit 1; \
	fi; \
	sed -i.bak "s|^PARKINSOUND_STEPGATE_VERSION = .*|PARKINSOUND_STEPGATE_VERSION = $$hash|" $(RECIPE_MK); \
	rm -f $(RECIPE_MK).bak; \
	echo "Recipe bumped to $$hash"; \
	echo "Stage with: git add $(RECIPE_MK)"
