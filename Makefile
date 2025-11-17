CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -Iinclude -Isrc
LDFLAGS = -lm

# Source files
CORE_SRC = src/core/runtime.c src/core/gc.c
FRONTEND_SRC = src/frontend/tokenizer.c src/frontend/parser.c
COMPILER_SRC = src/compiler/compiler.c
VM_SRC = src/vm/vm.c
MAIN_SRC = main.c

ALL_SRC = $(CORE_SRC) $(FRONTEND_SRC) $(COMPILER_SRC) $(VM_SRC) $(MAIN_SRC)

# Object files
OBJ = $(ALL_SRC:.c=.o)

# Output binary
TARGET = kronos

.PHONY: all clean run lsp

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

lsp: src/lsp/lsp_server.c src/core/runtime.o src/core/gc.o src/frontend/tokenizer.o src/frontend/parser.o src/compiler/compiler.o
	$(CC) $(CFLAGS) -o kronos-lsp src/lsp/lsp_server.c \
		src/core/runtime.o src/core/gc.o \
		src/frontend/tokenizer.o src/frontend/parser.o \
		src/compiler/compiler.o $(LDFLAGS)

clean:
	rm -f $(OBJ) $(TARGET) kronos-lsp
	rm -f src/core/*.o src/frontend/*.o src/compiler/*.o src/vm/*.o src/lsp/*.o

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./$(TARGET) examples/test.kr

# Install target (optional)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean run test install lsp

