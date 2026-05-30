CC      = cc
CFLAGS  = -Wall -Wextra -Werror -O2
TARGET  = libstellaris_fix.dylib

CHECKSUMS_FILE         = CHECKSUMS.txt
RELEASE_CHECKSUMS_FILE = RELEASE_CHECKSUMS.txt
RELEASE_ZIP            = stellaris-mac-fixes.zip
PROJECT_NAME           = stellaris-mac-fixes

# Files included in CHECKSUMS.txt (the manifest of what's in the release zip).
RELEASE_FILES = \
	libstellaris_fix.dylib \
	install.sh \
	uninstall.sh \
	Install.command \
	Uninstall.command \
	stellaris_fix.c \
	test_interpose.c \
	Makefile \
	README.md \
	LICENSE

.PHONY: all clean test test_decoder install uninstall checksums release help

help:
	@echo "Targets:"
	@echo "  make            Build the universal dylib"
	@echo "  make test       Build and run the test suite"
	@echo "  make install    Install into the Stellaris directory"
	@echo "  make uninstall  Remove from the Stellaris directory"
	@echo "  make checksums  Generate CHECKSUMS.txt and embed dylib hash in install.sh"
	@echo "  make release    Full release: build, checksum, zip, generate RELEASE_CHECKSUMS.txt"
	@echo "  make clean      Remove build artifacts"

all: $(TARGET)

$(TARGET): stellaris_fix.c
	$(CC) -dynamiclib -arch x86_64 -arch arm64 $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET) test_interpose $(CHECKSUMS_FILE) $(RELEASE_CHECKSUMS_FILE) $(RELEASE_ZIP)

test: $(TARGET) test_interpose.c test_decoder
	$(CC) -arch x86_64 $(CFLAGS) -Wno-unused-result -o test_interpose test_interpose.c
	DYLD_INSERT_LIBRARIES=./$(TARGET) STELLARIS_FIX_DEBUG=1 ./test_interpose
	@rm -f test_interpose

# Standalone decoder test (no dylib link; exercises the load-instruction
# pattern matcher used by Fix 4 against known-good byte sequences).
test_decoder: test_decoder.c
	$(CC) -O0 -arch x86_64 $(CFLAGS) -o test_decoder test_decoder.c
	./test_decoder
	@rm -f test_decoder

install: $(TARGET)
	./install.sh

uninstall:
	./uninstall.sh

# ── Checksum generation ──────────────────────────────────────────────────
#
# Computes MD5 of the built dylib, embeds it in install.sh (replacing
# whatever was there before — idempotent), then writes CHECKSUMS.txt
# with MD5 + SHA-256 of every file destined for the release zip.
#
checksums: $(TARGET)
	@echo "Computing dylib MD5..."
	@DYLIB_MD5=$$(md5 -q $(TARGET)); \
	echo "  $(TARGET) MD5: $$DYLIB_MD5"; \
	sed -i.bak "s|^EXPECTED_DYLIB_MD5=.*|EXPECTED_DYLIB_MD5=\"$$DYLIB_MD5\"|" install.sh && \
	rm -f install.sh.bak && \
	echo "  embedded into install.sh"
	@echo "Generating $(CHECKSUMS_FILE)..."
	@{ \
		echo "# stellaris-mac-fixes — file checksums"; \
		echo "#"; \
		echo "# Verify a single file:"; \
		echo "#   md5 -q FILENAME              (compare to MD5 below)"; \
		echo "#   shasum -a 256 FILENAME       (compare to SHA256 below)"; \
		echo "#"; \
		echo "# To verify the release zip itself, see RELEASE_CHECKSUMS.txt"; \
		echo "# (published alongside the release, not bundled inside)."; \
		echo ""; \
		echo "## MD5"; \
		for f in $(RELEASE_FILES); do \
			[ -f $$f ] && printf "%s  %s\n" "$$(md5 -q $$f)" "$$f"; \
		done; \
		echo ""; \
		echo "## SHA-256"; \
		for f in $(RELEASE_FILES); do \
			[ -f $$f ] && shasum -a 256 $$f; \
		done; \
	} > $(CHECKSUMS_FILE)
	@echo "  wrote $(CHECKSUMS_FILE)"

# ── Release build ────────────────────────────────────────────────────────
#
# Full pipeline: clean → build → checksums → zip → release-checksums.
# Output: $(RELEASE_ZIP) and $(RELEASE_CHECKSUMS_FILE).
#
release: clean all checksums
	@echo "Building $(RELEASE_ZIP)..."
	@rm -f /tmp/$(RELEASE_ZIP)
	@cd .. && zip -rq /tmp/$(RELEASE_ZIP) $(PROJECT_NAME) \
		-x '*.DS_Store' \
		   '$(PROJECT_NAME)/.*' \
		   '$(PROJECT_NAME)/*.zip' \
		   '$(PROJECT_NAME)/test_interpose' \
		   '$(PROJECT_NAME)/$(RELEASE_CHECKSUMS_FILE)'
	@mv /tmp/$(RELEASE_ZIP) .
	@echo "  wrote $(RELEASE_ZIP) ($$(du -h $(RELEASE_ZIP) | cut -f1))"
	@echo "Generating $(RELEASE_CHECKSUMS_FILE)..."
	@{ \
		echo "# stellaris-mac-fixes — release zip checksums"; \
		echo "#"; \
		echo "# Use these to verify your downloaded $(RELEASE_ZIP) BEFORE extracting."; \
		echo "# These hashes should be published alongside the release (e.g., on the"; \
		echo "# GitHub releases page) so users can verify the download is authentic."; \
		echo "#"; \
		echo "# To verify on macOS:"; \
		echo "#   md5 -q $(RELEASE_ZIP)         (compare to MD5 below)"; \
		echo "#   shasum -a 256 $(RELEASE_ZIP)  (compare to SHA256 below)"; \
		echo ""; \
		printf "MD5:    %s\n" "$$(md5 -q $(RELEASE_ZIP))"; \
		printf "SHA256: %s\n" "$$(shasum -a 256 $(RELEASE_ZIP) | awk '{print $$1}')"; \
		printf "Size:   %s bytes\n" "$$(stat -f %z $(RELEASE_ZIP))"; \
	} > $(RELEASE_CHECKSUMS_FILE)
	@echo ""
	@echo "════════════════════════════════════════════════════════"
	@echo "Release built: $(RELEASE_ZIP)"
	@cat $(RELEASE_CHECKSUMS_FILE)
	@echo "════════════════════════════════════════════════════════"
