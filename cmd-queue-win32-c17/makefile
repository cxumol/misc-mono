CC=zig cc
CFLAGS=-std=c17 -g -Wall -D_CRT_SECURE_NO_WARNINGS

all: cmd_queue_gui.exe

cmd_queue_gui.exe: main.c
	$(CC) $(CFLAGS) -o cmd_queue_gui.exe main.c -lgdi32 -luser32 -lkernel32

clean:
	rm -f cmd_queue_gui.exe
