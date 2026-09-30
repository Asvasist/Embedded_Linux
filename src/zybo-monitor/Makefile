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
OBJS := main.o board.o options.o

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c board.h options.h
	$(CC) $(CPPFLAGS) $(INCS) $(WARN) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

clean:
	rm -f $(PROG) $(OBJS) $(OBJS:.o=.d)

.PHONY: clean
