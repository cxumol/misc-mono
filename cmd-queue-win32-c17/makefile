CC = zig cc -target x86_64-windows-gnu
# CFLAGS for debug: -Wall -Wextra -std=c17 -g -DUNICODE -D_UNICODE
# CFLAGS for release: -Wall -Wextra -std=c17 -O2 -s -DUNICODE -D_UNICODE -DNDEBUG
CFLAGS = -Wall -Wextra -std=c17 -O2 -s -DUNICODE -D_UNICODE -DNDEBUG 
LDFLAGS = -mwindows
LIBS = -lgdi32 -luser32 -lkernel32 -lcomctl32 # comctl32 for InitCommonControlsEx if needed

TARGET = cmd_queue_win32.exe
SOURCES = main.c

OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(TARGET) *.stackdump

run: $(TARGET)
	./$(TARGET)
