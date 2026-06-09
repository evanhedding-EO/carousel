# carousel - pure-make build against SOEM 2.0 (no CMake required).
# Compiles SOEM's sources directly + our app, links into build/carousel.
#
#   make            build build/carousel
#   make clean      remove build/
#   make SOEM=/path/to/SOEM   override SOEM location (default: ../SOEM)

SOEM   ?= ../SOEM
BUILD  := build
CC     ?= cc

# our code: strict warnings. SOEM code: quiet (third-party).
APPFLAGS  := -std=gnu11 -Wall -Wextra -O2
SOEMFLAGS := -std=gnu11 -w -O2
CPPFLAGS  := -D_GNU_SOURCE -I. -Isoem_config \
             -I$(SOEM)/include -I$(SOEM)/include/soem \
             -I$(SOEM)/osal -I$(SOEM)/osal/linux \
             -I$(SOEM)/oshw -I$(SOEM)/oshw/linux
LDLIBS    := -pthread -lrt

APP_SRC  := main.c bus.c
SOEM_SRC := $(wildcard $(SOEM)/src/*.c) \
            $(SOEM)/osal/linux/osal.c \
            $(SOEM)/oshw/linux/nicdrv.c $(SOEM)/oshw/linux/oshw.c

APP_OBJ  := $(patsubst %.c,$(BUILD)/app/%.o,$(notdir $(APP_SRC)))
SOEM_OBJ := $(patsubst %.c,$(BUILD)/soem/%.o,$(notdir $(SOEM_SRC)))

vpath %.c . $(SOEM)/src $(SOEM)/osal/linux $(SOEM)/oshw/linux

.PHONY: all clean
all: $(BUILD)/carousel

$(BUILD)/carousel: $(APP_OBJ) $(SOEM_OBJ)
	$(CC) $(APPFLAGS) $^ -o $@ $(LDLIBS)
	@echo "built $@"

$(BUILD)/app/%.o: %.c | $(BUILD)/app
	$(CC) $(APPFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/soem/%.o: %.c | $(BUILD)/soem
	$(CC) $(SOEMFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/app $(BUILD)/soem:
	mkdir -p $@

clean:
	rm -rf $(BUILD)
