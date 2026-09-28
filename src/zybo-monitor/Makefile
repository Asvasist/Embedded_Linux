# SPDX-License-Identifier: MIT
#
# Buildroot passes CC/CFLAGS/LDFLAGS on the command line, so the project
# specific flags are kept in separate variables.
#
# Host build for quick checks:  make
# Cross build:                  make CC=arm-linux-gnueabihf-gcc

CFLAGS ?= -O2 -g
WARN   := -Wall -Wextra -Wshadow -Wformat=2
INCS   := -I../zybo-btn

PROG := zybo-monitor
OBJS := main.o board.o

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c board.h
	$(CC) $(CPPFLAGS) $(INCS) $(WARN) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(PROG) $(OBJS)

.PHONY: clean
