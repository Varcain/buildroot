LVGL_SRCS := $(shell find src demos -name '*.c')
LVGL_OBJS := $(patsubst %.c,.hammer-build/%.o,$(LVGL_SRCS))

CPPFLAGS += -DLV_CONF_INCLUDE_SIMPLE -I.
CFLAGS += -std=gnu11 -Wall -Wextra
LDLIBS += -lm

.PHONY: all clean

all: lvmusic hammer-uinput hammer-sqlite linux-rt-latency

lvmusic: .hammer-build/hammer-src/lvmusic.o $(LVGL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

hammer-uinput: .hammer-build/hammer-src/hammer-uinput.o
	$(CC) $(LDFLAGS) -o $@ $^

hammer-sqlite: .hammer-build/hammer-src/hammer-sqlite.o
	$(CC) $(LDFLAGS) -o $@ $^ -lsqlite3

linux-rt-latency: .hammer-build/hammer-src/linux-rt-latency.o
	$(CC) $(LDFLAGS) -o $@ $^

.hammer-build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	$(RM) -r .hammer-build lvmusic hammer-uinput hammer-sqlite linux-rt-latency
