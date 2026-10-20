# TbAccel.kext - macOS Thunderbolt/USB4 tbaccel protocol driver
#
# Builds an IOKit kext for the running kernel architecture (arm64e on Apple
# Silicon). For Intel hosts change ARCHS to "-arch x86_64".
# The private IOThunderboltFamily headers are reconstructed in Private/.
#
#   make            # build build/TbAccel.kext
#   make load       # kextutil -l (developer load; SIP must allow it)
#   make unload
#   make clean
#
# SPDX-License-Identifier: GPL-2.0

KEXT        := TbAccel
BUNDLE_ID   := com.phh.tbaccel
BUILD       := build
KEXTDIR     := $(BUILD)/$(KEXT).kext
# Sign with the local self-signed identity if it exists (ad-hoc is rejected by
# kmutil on Apple Silicon). Create it with scripts/make-signing-cert.sh.
SIGN_ID     ?= TbAccel Local
KEYCHAIN    ?= $(HOME)/Library/Keychains/tbaccel.keychain-db
# dev-only local signing keychain; the password is not a secret (see
# scripts/make-signing-cert.sh). The keychain locks on reboot and is not a
# login keychain, so the build unlocks it before signing.
KEYCHAIN_PW ?= tbaccel
ifeq ($(wildcard $(KEYCHAIN)),)
CODESIGN     = codesign --force --deep --sign -
UNLOCK       = @true
else
CODESIGN     = codesign --force --deep --keychain "$(KEYCHAIN)" --sign "$(SIGN_ID)"
UNLOCK       = @security unlock-keychain -p "$(KEYCHAIN_PW)" "$(KEYCHAIN)" 2>/dev/null || true
endif
SDK         := $(shell xcrun --show-sdk-path)
CXX         := $(shell xcrun -f clang++)
KERNEL_HDRS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
ARCHS       := -arch arm64e
INCLUDES    := -I Shared -I Private -I Kernel -I $(KERNEL_HDRS)
CXXFLAGS    := $(ARCHS) -isysroot $(SDK) -std=gnu++17 -fno-exceptions -fno-rtti \
               -fapple-kext -fno-builtin -mkernel -O2 -Wall \
               -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
               $(INCLUDES) -Wno-unused-parameter -Wno-inconsistent-missing-override
LDFLAGS     := $(ARCHS) -isysroot $(SDK) -nostdlib -Xlinker -kext \
               -lkmodc++ -lkmod -lcc_kext

SRCS        := Kernel/TbAccelClient.cpp \
               Kernel/TbAccelUserClient.cpp \
               Kernel/TbAccelService.cpp \
               Kernel/TbAccelDriver.cpp

OBJS        := $(patsubst Kernel/%.cpp,$(BUILD)/%.o,$(SRCS))
HDRS        := $(wildcard Kernel/*.h Private/*.h Shared/*.h)

.PHONY: all clean load unload user cert

all: $(KEXTDIR)

$(BUILD)/%.o: Kernel/%.cpp $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(KEXTDIR): $(OBJS) Info.plist
	@mkdir -p $(KEXTDIR)/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXTDIR)/Contents/MacOS/$(KEXT) $(OBJS)
	cp Info.plist $(KEXTDIR)/Contents/Info.plist
	$(UNLOCK)
	$(CODESIGN) $(KEXTDIR)
	@echo "built $(KEXTDIR)"

load: $(KEXTDIR)
	sudo kextutil -l $(KEXTDIR)

unload:
	sudo kextutil -u $(BUNDLE_ID)

user:
	clang -I Shared -Wall User/tbaccel.c -framework IOKit \
	    -framework CoreFoundation -o $(BUILD)/tbaccel

cert:
	scripts/make-signing-cert.sh

# Build with the Xcode Kernel Extension target (generates the kext module glue).
xc:
	security unlock-keychain -p "$(KEYCHAIN_PW)" "$(KEYCHAIN)" 2>/dev/null || true
	xcodebuild -project TbAccel.xcodeproj -configuration Release -target $(KEXT) \
	    CONFIGURATION_BUILD_DIR="$(CURDIR)/build_xc" build

clean:
	rm -rf $(BUILD)
