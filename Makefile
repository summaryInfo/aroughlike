NAME ?= game

CFLAGS= -O3 -flto -march=native -g -pthread

CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CFLAGS += -Wno-unknown-warning -Wno-unknown-warning-option
CFLAGS += -Walloca -Wno-aggressive-loop-optimizations
CFLAGS += -Wdisabled-optimization -Wduplicated-branches -Wduplicated-cond
CFLAGS += -Wignored-attributes  -Wincompatible-pointer-types
CFLAGS += -Winit-self -Wwrite-strings -Wvla
CFLAGS += -Wmissing-format-attribute -Wmissing-noreturn
CFLAGS += -Wswitch-bool -Wpacked -Wshadow -Wformat-security
CFLAGS += -Wswitch-unreachable -Wlogical-op
CFLAGS += -Wnested-externs -Wstrict-prototypes

OBJ := window.o image.o game.o tilemap.o worker.o generator.o

LIBS != pkg-config xcb xcb-shm --libs
INCLUES != pkg-config xcb xcb-shm --cflags

LDLIBS += -lrt -lm $(LIBS)
CFLAGS += $(INCLUES)

all: $(NAME)

run: all
	./$(NAME)

clean:
	rm -rf *.o $(NAME)

force: clean
	$(MAKE) all

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(LDLIBS) -o $@

window.o: image.h util.h context.h worker.h keys.h
image.o: image.h util.h stb_image.h worker.h
game.o: context.h util.h keys.h tilemap.h worker.h
generator.o: util.h
tilemap.o: image.h tilemap.h util.h

.PHONY: all clean force run
