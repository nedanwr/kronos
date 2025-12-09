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

# LSP server source files (split from lsp_server.c)
LSP_SERVER_SRC = src/lsp/lsp_main.c \
                 src/lsp/lsp_messages.c \
                 src/lsp/lsp_utils.c \
                 src/lsp/lsp_diagnostics.c \
                 src/lsp/lsp_handlers.c \
                 src/lsp/lsp_completion.c \
                 src/lsp/lsp_hover.c \
                 src/lsp/lsp_definition.c
LSP_SERVER_OBJ = $(LSP_SERVER_SRC:.c=.o)

# Dependency files (auto-generated)
DEP = $(OBJ:.o=.d)
LSP_DEP = $(LSP_SERVER_OBJ:.o=.d)

# Output binary
TARGET = kronos

.PHONY: all clean run test test-unit test-lsp install lsp

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

lsp: $(LSP_SERVER_OBJ) $(LSP_OBJ)
	$(CC) $(CFLAGS) -o kronos-lsp $^ $(LDFLAGS)

clean:
	rm -f $(OBJ) $(DEP) $(TARGET) kronos-lsp
	rm -f src/core/*.o src/core/*.d src/frontend/*.o src/frontend/*.d
	rm -f src/compiler/*.o src/compiler/*.d src/vm/*.o src/vm/*.d src/lsp/*.o src/lsp/*.d
	rm -f $(TEST_OBJ) $(TEST_DEP) $(TEST_TARGET)
	rm -f tests/framework/*.o tests/framework/*.d tests/unit/*.o tests/unit/*.d

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	./$(TARGET) examples/test.kr

# Unit test sources
TEST_FRAMEWORK_SRC = tests/framework/test_framework.c
TEST_UNIT_SRC = tests/unit/test_tokenizer.c \
                tests/unit/test_parser.c \
                tests/unit/test_runtime.c \
                tests/unit/test_compiler.c \
                tests/unit/test_vm.c \
                tests/unit/test_gc.c \
                tests/unit/test_main.c

# Unit test object files
TEST_OBJ = $(TEST_FRAMEWORK_SRC:.c=.o) $(TEST_UNIT_SRC:.c=.o)
TEST_DEP = $(TEST_OBJ:.o=.d)

# Unit test executable
TEST_TARGET = tests/unit/kronos_unit_tests

# Object files for unit tests (exclude main.o)
TEST_OBJ_SRC = $(CORE_SRC) $(FRONTEND_SRC) $(COMPILER_SRC) $(VM_SRC)
TEST_OBJ_BASE = $(TEST_OBJ_SRC:.c=.o)

# Build unit tests
$(TEST_TARGET): $(TEST_OBJ_BASE) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Build test object files
tests/framework/%.o: tests/framework/%.c
	$(CC) $(CFLAGS) -c $< -o $@

tests/unit/%.o: tests/unit/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Run unit tests
test-unit: $(TEST_TARGET)
	./$(TEST_TARGET)

# LSP test sources
TEST_LSP_SRC = tests/lsp/test_lsp_framework.c tests/lsp/test_lsp_features.c
TEST_LSP_OBJ = $(TEST_LSP_SRC:.c=.o)
TEST_LSP_TARGET = tests/lsp/kronos_lsp_tests

# Build LSP tests
test-lsp: lsp $(TEST_LSP_TARGET)
	./$(TEST_LSP_TARGET)

$(TEST_LSP_TARGET): tests/lsp/test_lsp_main.c $(TEST_LSP_OBJ) $(TEST_FRAMEWORK_SRC:.c=.o) $(TEST_OBJ_BASE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

tests/lsp/%.o: tests/lsp/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Install target (optional)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Include auto-generated dependency files (if they exist)
-include $(DEP)
-include $(LSP_DEP)
-include $(TEST_DEP)
