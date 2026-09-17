REPO_ROOT := $(abspath ..)
TARGETS := main.bin

# Cue-driven engine test, and the fixture channel prober. Each has its own
# notmain(), links against the same COMMON_PIOBJS as main.bin, and both are
# kept out of `all` so the default build and the top-level Makefile still
# produce main.bin only.
TEST_TARGETS := enginetest.bin findchan.bin

LDLIBS ?=

# Find all .c files recursively
SRCC := $(shell find $(REPO_ROOT)/pos/src -name '*.c')
SRCS := $(shell find $(REPO_ROOT)/pos/src -name '*.S')

PIOBJS := $(SRCC:.c=.pi.o)
PIOBJS += $(SRCS:.S=.pi.o)

COMMON_PIOBJS = $(shell find $(REPO_ROOT) -type f -name '*.pi.o' -not -path '*/pos/*')
COMMON_PIOBJS += $(PIOBJS)

include $(REPO_ROOT)/scripts/Makefile.gcc
include $(REPO_ROOT)/scripts/Makefile.ld

all: $(TARGETS) $(PIOBJS)

enginetest: enginetest.bin

findchan: findchan.bin

clean:
	rm -f $(TARGETS) $(TEST_TARGETS) $(COMMON_PIOBJS)
	rm -f main.pi.o main.pi.elf main.list
	rm -f enginetest.pi.o enginetest.pi.elf enginetest.list
	rm -f findchan.pi.o findchan.pi.elf findchan.list

.PHONY: all clean enginetest findchan
