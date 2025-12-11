CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -Iinclude -Isrc -MMD -MP
LDFLAGS = -lm

# Source files
CORE_SRC = src/core/runtime.c src/core/gc.c
FRONTEND_SRC = src/frontend/tokenizer.c src/frontend/keywords_hash.c src/frontend/parser.c
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

# Generate perfect hash for keywords using gperf
src/frontend/keywords_hash.c: src/frontend/keywords.gperf
	gperf -t --output-file=$@ --lookup-function-name=keyword_lookup --compare-strncmp $<
	@# Fix empty string entries in generated file
	@python3 -c "import re; content = open('$@').read(); content = re.sub(r'\{\s*\"\"\s*\}', '{NULL, 0}', content); open('$@', 'w').write(content)"
	@# Remove inline keywords and related preprocessor directives that can cause linking issues
	@python3 scripts/fix_keywords_hash.py $@
	@# Fix struct definition and function signature
	@# Fix function signatures (old-style to modern C)
	@# Cross-platform sed: macOS needs -i '', Linux needs -i
	@if [ "$$(uname)" = "Darwin" ]; then \
		sed -i '' 's/^static unsigned int$$/static unsigned int/' $@; \
		sed -i '' 's/^hash (str, len)$$/keyword_hash_func (register const char *str, register unsigned int len)/' $@; \
		sed -i '' '/^     register const char \*str;$$/d' $@; \
		sed -i '' '/^     register unsigned int len;$$/d' $@; \
		sed -i '' 's/^struct KeywordEntry \*/const struct KeywordEntry */' $@; \
		sed -i '' 's/^keyword_lookup (str, len)$$/keyword_lookup (register const char *str, register unsigned int len)/' $@; \
		sed -i '' 's/static struct KeywordEntry wordlist/static const struct KeywordEntry wordlist/' $@; \
		sed -i '' 's/unsigned int key = hash (str, len);/unsigned int key = keyword_hash_func (str, len);/' $@; \
		sed -i '' 's/register const char \*s = wordlist\[key\]\.name;/register const struct KeywordEntry *entry = \&wordlist[key];/' $@ || true; \
		sed -i '' 's/if (\*str == \*s && !strncmp (str + 1, s + 1, len - 1) && s\[len\] == '\''\\0'\'')/if (entry->keyword \&\& strlen(entry->keyword) == len \&\& !strncmp(str, entry->keyword, len))/' $@ || true; \
		sed -i '' 's/return \&wordlist\[key\];/return entry;/' $@ || true; \
		sed -i '' 's/return 0;/return NULL;/' $@ || true; \
	else \
		sed -i 's/^static unsigned int$$/static unsigned int/' $@; \
		sed -i 's/^hash (str, len)$$/keyword_hash_func (register const char *str, register unsigned int len)/' $@; \
		sed -i '/^     register const char \*str;$$/d' $@; \
		sed -i '/^     register unsigned int len;$$/d' $@; \
		sed -i 's/^struct KeywordEntry \*/const struct KeywordEntry */' $@; \
		sed -i 's/^keyword_lookup (str, len)$$/keyword_lookup (register const char *str, register unsigned int len)/' $@; \
		sed -i 's/static struct KeywordEntry wordlist/static const struct KeywordEntry wordlist/' $@; \
		sed -i 's/unsigned int key = hash (str, len);/unsigned int key = keyword_hash_func (str, len);/' $@; \
		sed -i 's/register const char \*s = wordlist\[key\]\.name;/register const struct KeywordEntry *entry = \&wordlist[key];/' $@ || true; \
		sed -i 's/if (\*str == \*s && !strncmp (str + 1, s + 1, len - 1) && s\[len\] == '\''\\0'\'')/if (entry->keyword \&\& strlen(entry->keyword) == len \&\& !strncmp(str, entry->keyword, len))/' $@ || true; \
		sed -i 's/return \&wordlist\[key\];/return entry;/' $@ || true; \
		sed -i 's/return 0;/return NULL;/' $@ || true; \
	fi
	@# Keep struct definition but ensure it's properly formatted
	@echo "  Generated $@ from $<"

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
