CFLAGS_uniwill-acpi.o := -DDEBUG
CFLAGS_uniwill-wmi.o := -DDEBUG
CFLAGS_uniwill-ite8291.o := -DDEBUG
obj-m += uniwill-laptop.o
uniwill-laptop-y := uniwill-acpi.o uniwill-wmi.o uniwill-ite8291.o
HOSTCC ?= cc
UNIWILL_SOURCE_DIR := $(if $(src),$(src),.)
UNIWILL_VERSION ?= $(strip $(shell cat $(UNIWILL_SOURCE_DIR)/VERSION))
UNIWILL_BUILD_NUMBER ?= $(strip $(shell cat $(UNIWILL_SOURCE_DIR)/BUILD_NUMBER))
CFLAGS_uniwill-acpi.o += -DUNIWILL_MODULE_VERSION=\"$(UNIWILL_VERSION)\"
HOSTCFLAGS ?= -O2 -g -Wall -Wextra -std=c11 -pthread
HOSTCFLAGS += $(shell pkg-config --cflags blockdev 2>/dev/null)
HOSTCFLAGS += -DUNIWILLD_VERSION=\"$(UNIWILL_VERSION)\"
HOSTCFLAGS += -DUNIWILLD_BUILD_NUMBER=\"$(UNIWILL_BUILD_NUMBER)\"
HOSTLDLIBS ?= $(shell pkg-config --libs libsystemd 2>/dev/null || echo -lsystemd) -pthread
HOSTLDLIBS += $(shell pkg-config --libs blockdev 2>/dev/null) -lbd_fs
TOUCHPAD_CFLAGS ?= $(shell pkg-config --cflags gio-2.0)
TOUCHPAD_LDLIBS ?= $(shell pkg-config --libs gio-2.0)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
UNIWILL_OS_ID ?= $(strip $(shell . /etc/os-release 2>/dev/null && printf '%s' "$$ID"))
ifeq ($(UNIWILL_OS_ID),cachyos)
LLVM ?= 1
endif
KBUILD_LLVM_ARG := $(if $(strip $(LLVM)),LLVM=$(LLVM))

.PHONY: all service test check-version clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KBUILD_LLVM_ARG) modules

service: uniwilld uniwill-touchpad-sync

test: uniwilld-test
	./uniwilld-test

check-version:
	./scripts/check-version.sh

uniwilld: uniwilld.c VERSION BUILD_NUMBER
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $< $(HOSTLDLIBS)

uniwill-touchpad-sync: uniwill-touchpad-sync.c
	$(HOSTCC) $(HOSTCFLAGS) $(TOUCHPAD_CFLAGS) -o $@ $< $(TOUCHPAD_LDLIBS)

uniwilld-test: uniwilld-test.c uniwilld.c VERSION BUILD_NUMBER
	$(HOSTCC) $(HOSTCFLAGS) -o $@ uniwilld-test.c $(HOSTLDLIBS)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $(KBUILD_LLVM_ARG) clean
	$(RM) uniwilld uniwilld-test uniwill-touchpad-sync
