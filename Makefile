CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -Iinclude -Isrc -MMD -MP
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

# LSP server excludes main.c and vm (only needs compiler/frontend)
LSP_SRC = $(CORE_SRC) $(FRONTEND_SRC) $(COMPILER_SRC)
LSP_OBJ = $(LSP_SRC:.c=.o)

# Dependency files (auto-generated)
DEP = $(OBJ:.o=.d)

# Output binary
TARGET = kronos

.PHONY: all clean run test install lsp

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

lsp: src/lsp/lsp_server.c $(LSP_OBJ)
	$(CC) $(CFLAGS) -o kronos-lsp $^ $(LDFLAGS)

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) kronos-lsp
	rm -f src/core/*.o src/core/*.d src/frontend/*.o src/frontend/*.d
	rm -f src/compiler/*.o src/compiler/*.d src/vm/*.o src/vm/*.d src/lsp/*.o src/lsp/*.d

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./$(TARGET) examples/test.kr

# Install target (optional)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
# Include auto-generated dependency files (if they exist)
-include $(DEP)
