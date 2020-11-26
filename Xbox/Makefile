XBE_TITLE = Odamex
XBE_TITLEID = 0x4F444D58
GEN_XISO = $(XBE_TITLE).iso
NXDK_DIR ?= $(CURDIR)/nxdk
NXDK_SDL = y
NXDK_SDL_MIXER = y
NXDK_CXX = y
NXDK_NET = y
NXDK_STACKSIZE = 1048576

SRCS = \
	$(wildcard *.cpp) \
	$(wildcard ../client/sdl/*.cpp) \
	$(wildcard ../client/src/*.cpp) \
	$(wildcard ../common/*.cpp)

GIT_DESCRIBE := ../common/git_describe.h

CFLAGS := -DUSE_PNG -DCLIENT_APP -D_XBOX -DGCONSOLE -DNOASM -D_WIN32 -DWIN32 \
	-I../common -I../client/src -I../client/sdl -I./ \
	-Wno-deprecated-declarations -mmmx -msse

ifneq ($(DEBUG),y)
	CFLAGS += -DNDEBUG -O3
endif

CXXFLAGS := $(CFLAGS)

default: $(GIT_DESCRIBE) cp_data all

$(GIT_DESCRIBE):
	@echo "#define GIT_DESCRIBE \"xbox-test\"" > $@

cp_data:
	@mkdir -p $(OUTPUT_DIR)
	@cp ../wad/odamex.wad $(OUTPUT_DIR)/

CLEANRULES += clean-odamex

.PHONY: clean-odamex cp-data
clean-odamex:
	@rm -f $(GIT_DESCRIBE)

include $(NXDK_DIR)/Makefile
