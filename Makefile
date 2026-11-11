# Builds build/libRBMod.dylib (arm64 iOS).
#  - On macOS with Xcode (GitHub Actions):  make
#  - On Linux with clang + ld64.lld:         make LINUX=1   (links against tools/libSystem.tbd)

SRC     := src/rb_runtime.c src/hooks.c src/ui.c src/input.c src/cpuqb.c src/defense.c src/brain.c src/dbrain.c src/camera3d.c src/defcall.c src/offtools.c src/rules.c
OUT     := build/libRBMod.dylib
INSTALL := @executable_path/Frameworks/libRBMod.dylib
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -fno-stack-protector -fvisibility=hidden -fblocks

ifdef LINUX
CC      := clang -target arm64-apple-ios15.0 -ffreestanding -DRB_FREESTANDING
LINK    := ld64.lld -arch arm64 -platform_version ios 15.0 15.0 -dylib -fixup_chains -install_name $(INSTALL) tools/libSystem.tbd tools/libobjc.tbd
else
SDK     := $(shell xcrun --sdk iphoneos --show-sdk-path)
CC      := xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=15.0 -isysroot $(SDK)
endif

OBJ := $(SRC:src/%.c=build/%.o)

all: $(OUT)

build/%.o: src/%.c src/rb.h src/sys.h src/ui.h src/input.h src/cpuqb.h src/defense.h src/brain.h src/dbrain.h src/camera3d.h src/defcall.h src/offtools.h src/tune.h src/rules.h src/tune_table.inc src/hooks.def
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJ)
ifdef LINUX
	$(LINK) -o $@ $(OBJ)
else
	$(CC) -dynamiclib -install_name $(INSTALL) -o $@ $(OBJ) -lobjc
endif

clean:
	rm -rf build

.PHONY: all clean
