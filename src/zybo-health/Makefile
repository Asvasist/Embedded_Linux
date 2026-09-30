CFLAGS ?= -O2
WARN = -Wall -Wextra -Werror -Wformat=2

zybo-health: main.c health.c health.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(LDFLAGS) -o $@ main.c health.c

clean:
	rm -f zybo-health

.PHONY: clean
