OUT      ?= build
RELEASES ?= releases

MESON     ?= meson
NINJA     ?= ninja
TAR       ?= tar
CONTAINER ?= $(shell command -v podman 2>/dev/null || command -v docker 2>/dev/null || echo podman)

ifeq ($(filter grouped-target,$(.FEATURES)),)
$(error GNU make 4.3+ required)
endif

VERSION := $(strip $(shell cat RELEASE))

ARCHES ?= 64 32

CROSSFILE_DIR ?= .
CROSSFILE_64  ?= build-win64.txt
CROSSFILE_32  ?= build-win32.txt

ARCHDIR_64 ?= x64
ARCHDIR_32 ?= x32

BUILDTYPE ?= release
NDEBUG    ?= if-release
BUILD_ID  ?= false
STRIP     ?= 1

MESON_OPTS    ?=
MESON_OPTS_64 ?=
MESON_OPTS_32 ?=

MESON_STRIP := $(if $(filter 1,$(STRIP)),--strip,)

DIST_DIR  ?= $(OUT)/dist
DIST_NAME ?= dxvk-sarek-$(VERSION)
DIST      := $(DIST_DIR)/$(DIST_NAME)

CONTAINER_BASE ?= registry.gitlab.steamos.cloud/proton/sniper/sdk:latest
CONTAINER_OUT  ?= $(OUT)/container
CONTAINER_ARGS ?= CROSSFILE_DIR=.github/crossfiles \
                  CROSSFILE_64=steamrt-x86_64.txt \
                  CROSSFILE_32=steamrt-i386.txt

SOURCES := meson.build meson_options.txt version.h.in \
  $(shell find src include -type f -not -path 'include/vulkan/*' -not -path 'include/spirv/*' 2>/dev/null)

CROSSFILES   := $(foreach a,$(ARCHES),$(CROSSFILE_DIR)/$(CROSSFILE_$(a)))
BUILD_STAMPS := $(foreach a,$(ARCHES),$(OUT)/.built-$(a))

DIST_FILES := meson.build meson_options.txt version.h.in RELEASE \
              build-win32.txt build-win64.txt dxvk.conf \
              Makefile LICENSE README.md
DIST_TREES := src include .github
SUBMODULES := include/vulkan include/spirv

NO_SUDO = @test -z "$$SUDO_USER" || { echo "error: do not build with sudo — run 'make' as your user"; exit 1; }

.DELETE_ON_ERROR:

.PHONY: all x64 x32 dist release release-container clean help

all: $(BUILD_STAMPS)

x64:     $(OUT)/.built-64
x32:     $(OUT)/.built-32
dist:    $(OUT)/.dist
release: $(RELEASES)/$(DIST_NAME).tar.gz

$(OUT) $(RELEASES):
	@mkdir -p $@

$(OUT)/build.%/build.ninja: $(CROSSFILES) Makefile | $(OUT)
	$(NO_SUDO)
	rm -rf $(@D)
	$(MESON) setup --cross-file $(CROSSFILE_DIR)/$(CROSSFILE_$*) \
	  --buildtype $(BUILDTYPE)                                   \
	  --prefix $(abspath $(OUT))                                 \
	  $(MESON_STRIP)                                             \
	  --bindir $(ARCHDIR_$*)                                     \
	  --libdir $(ARCHDIR_$*)                                     \
	  -Db_ndebug=$(NDEBUG)                                       \
	  -Dbuild_id=$(BUILD_ID)                                     \
	  $(MESON_OPTS) $(MESON_OPTS_$*)                             \
	  $(@D)

$(OUT)/.built-%: $(OUT)/build.%/build.ninja $(SOURCES)
	$(NO_SUDO)
	$(NINJA) -C $(<D) install
	find $(OUT)/$(ARCHDIR_$*) -type f ! -name '*.dll' -delete
	@touch $@

$(OUT)/.dist: $(BUILD_STAMPS) $(DIST_FILES) | $(OUT)
	rm -rf $(DIST)
	mkdir -p $(DIST)/build
	$(foreach f,$(DIST_FILES),install -Dm644 $(f) $(DIST)/$(f) &&) :
	cp -r $(DIST_TREES) $(DIST)/
	rm -rf $(foreach s,$(SUBMODULES),$(DIST)/$(s))
	$(foreach a,$(ARCHES),cp -r $(OUT)/$(ARCHDIR_$(a)) $(DIST)/build/ &&) :
	@touch $@

$(RELEASES)/$(DIST_NAME).tar.gz: $(OUT)/.dist | $(RELEASES)
	$(TAR) -czf $@ -C $(DIST_DIR) $(DIST_NAME)

release-container:
	$(CONTAINER) run --rm -v "$(CURDIR):/src:z" -w /src \
	  --user "$$(id -u):$$(id -g)" \
	  -e HOME=/tmp \
	  $(CONTAINER_BASE) make release OUT=$(CONTAINER_OUT) $(CONTAINER_ARGS)

clean:
	rm -rf $(OUT) $(RELEASES)

help:
	@echo "make                    dlls for every arch in ARCHES ($(ARCHES))"
	@echo "make x64                64-bit dlls into $(OUT)/$(ARCHDIR_64)"
	@echo "make x32                32-bit dlls into $(OUT)/$(ARCHDIR_32)"
	@echo "make dist               source + build tree in $(DIST_DIR)/"
	@echo "make release            tarball into $(RELEASES)/ (host toolchain)"
	@echo "make release-container  the same, built inside $(CONTAINER_BASE)"
	@echo "make clean"
	@echo ""
	@echo "no install target: dxvk dlls are set up per wine prefix, not system wide"
