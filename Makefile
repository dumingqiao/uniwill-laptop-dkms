CFLAGS_uniwill-acpi.o := -DDEBUG
CFLAGS_uniwill-wmi.o := -DDEBUG
CFLAGS_uniwill-ite8291.o := -DDEBUG
obj-m += uniwill-laptop.o
uniwill-laptop-y := uniwill-acpi.o uniwill-wmi.o uniwill-ite8291.o
HOSTCC ?= cc
VERSION ?= $(shell cat VERSION)
BUILD_NUMBER ?= $(shell cat BUILD_NUMBER)
HOSTCFLAGS ?= -O2 -g -Wall -Wextra -std=c11 -pthread
HOSTCFLAGS += $(shell pkg-config --cflags blockdev 2>/dev/null)
HOSTCFLAGS += -DUNIWILLD_VERSION=\"$(VERSION)\" -DUNIWILLD_BUILD_NUMBER=\"$(BUILD_NUMBER)\"
HOSTLDLIBS ?= $(shell pkg-config --libs libsystemd 2>/dev/null || echo -lsystemd) -pthread
HOSTLDLIBS += $(shell pkg-config --libs blockdev 2>/dev/null) -lbd_fs
TOUCHPAD_CFLAGS ?= $(shell pkg-config --cflags gio-2.0)
TOUCHPAD_LDLIBS ?= $(shell pkg-config --libs gio-2.0)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
LLVM ?= 1

all:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules

service: uniwilld uniwill-touchpad-sync

test: uniwilld-test
	./uniwilld-test

uniwilld: uniwilld.c VERSION BUILD_NUMBER
	$(HOSTCC) $(HOSTCFLAGS) -o $@ $< $(HOSTLDLIBS)

uniwill-touchpad-sync: uniwill-touchpad-sync.c
	$(HOSTCC) $(HOSTCFLAGS) $(TOUCHPAD_CFLAGS) -o $@ $< $(TOUCHPAD_LDLIBS)

uniwilld-test: uniwilld-test.c uniwilld.c VERSION BUILD_NUMBER
	$(HOSTCC) $(HOSTCFLAGS) -o $@ uniwilld-test.c $(HOSTLDLIBS)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) clean
	$(RM) uniwilld uniwilld-test uniwill-touchpad-sync
