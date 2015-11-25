CC					= gcc
CFLAGS			= -g -pthread -Wall -Wextra -std=gnu99
C_FILES			= $(wildcard monitor/*.c) $(wildcard include/*.c)
OBJ_FILES		= $(filter-out obj/watchdog.o, $(addprefix obj/,$(notdir $(C_FILES:.c=.o))))
VPATH 			= monitor:include
MONITOR			= bin/monitor
WATCHDOG		= bin/watchdog
DIRS				= bin obj

.PHONY: clean directories

all: directories $(MONITOR) $(WATCHDOG)

$(MONITOR): $(OBJ_FILES)
	$(CC) $(CFLAGS) -o $@ $^

$(WATCHDOG): monitor/watchdog.c
	$(CC) $(CFLAGS) -o $@ $<

obj/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

directories: $(DIRS)

$(DIRS):
	mkdir -p $(DIRS)

clean:
	rm -rf obj
	rm bin/watchdog
	rm bin/monitor
