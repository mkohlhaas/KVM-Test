# Build for the /dev/kvm API sample.
#
# Usage:
#   make            build kvmtest
#   make run        build, then run it (needs access to /dev/kvm)
#   make clean      remove the binary
#
# CC and CFLAGS can be overridden, e.g. `make CC=clang` or
# `make CFLAGS='-O2 -Wall -Wextra'`.

TARGET := kvmtest
SRCS   := kvmtest.c

# CC is not assigned here: make's built-in default is already `cc`, which on
# most distributions is a symlink to gcc. Override it as `make CC=clang`.
CFLAGS ?= -Wall -Wextra

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) a.out
