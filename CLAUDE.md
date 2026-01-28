# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Kronos is a programming language written in C with human-readable syntax. It features a bytecode VM, reference-counting garbage collection, and an LSP server for IDE support.

## Build Commands

```bash
make                    # Build main binary (kronos)
make lsp                # Build LSP server (kronos-lsp)
make clean              # Remove build artifacts
make test-unit          # Run unit tests
./scripts/run_tests.sh  # Run full test suite (129 tests)
```

Run a single file:
```bash
./kronos examples/hello.kr
./kronos tests/integration/pass/variables_immutable.kr
```

Start the REPL:
```bash
./kronos
```

## Architecture

The project follows a classic compiler pipeline:

```
Source (.kr) → Tokenizer → Parser → Compiler → VM
```

### Directory Structure

- `src/core/` - Runtime value system (`KronosValue`) and reference-counting GC
- `src/frontend/` - Tokenizer and recursive descent parser (produces AST)
- `src/compiler/` - AST to bytecode compilation (60+ opcodes)
- `src/vm/` - Stack-based bytecode VM execution
- `src/lsp/` - Language Server Protocol implementation
- `main.c` - CLI entry point (file execution + REPL)

### Key Data Flow

1. **Tokenizer** (`tokenizer.c`) converts source to token stream
2. **Parser** (`parser.c`) builds AST from tokens (40+ node types)
3. **Compiler** (`compiler.c`) generates bytecode + constant pool
4. **VM** (`vm.c`) executes bytecode on a 1024-slot value stack

### Value System

`KronosValue` (in `runtime.h`) is a reference-counted union type supporting: numbers, strings, booleans, nil, functions, lists, ranges, maps, and channels.

## Feature Implementation Workflow

**Follow these steps strictly when implementing new features.**

### Step 1: Planning

Before writing any code, plan how to tackle the feature. Determine the order of events and which parts to implement first. Always prioritize the best and most efficient approach, NOT ease of implementation.

For the core language, this typically means modifying in order:
- `src/frontend/tokenizer.c/h` - Add token types if needed
- `src/frontend/parser.c/h` - Extend AST node types
- `src/compiler/compiler.c/h` - Generate bytecode
- `src/vm/vm.c/h` - Implement execution

### Step 2: Implementation Review

After implementing the feature, review the work **twice**, looking for:
- Bugs or logic errors
- Inefficiencies or unnecessary resource usage
- Opportunities to achieve the same result with less memory/CPU

The language must not be a resource hog. If any changes are made during review, repeat Step 2 (review twice again) until no more issues are found.

### Step 3: Core Tests

Add test cases in `tests/integration/pass/` and `tests/integration/fail/` to test the implementation. Run the tests. If tests fail:
1. Investigate why
2. Implement fixes
3. Return to Step 2 (review twice)
4. Only skip Step 3 if no additional tests are needed

Repeat until all tests pass.

### Step 4: LSP Implementation

Modify the LSP (`src/lsp/`) to support the new feature. The LSP is one of the trickiest components to get right.

After modifying the LSP, review your work **three times**, checking for bugs, inefficiencies, and anything missed. If any issues are found and fixed, review **three times again**. Repeat as needed until the review is clean.

### Step 5: LSP Tests

Create test cases for the LSP implementation in `tests/lsp/`. If tests fail:
1. Return to Step 4 and follow it exactly
2. Repeat Steps 4 and 5 until all LSP tests pass

### Step 6: Memory Checks

Run valgrind memory checks to detect leaks, invalid reads/writes, and other memory issues. Use `act` or `docker` to run valgrind:

```bash
# Using act to run the memory-check workflow locally
act -j memory-check

# Or using docker directly
docker run --rm -v $(pwd):/workspace -w /workspace gcc:latest bash -c \
  "apt-get update && apt-get install -y valgrind && make clean && make && \
   valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
   ./kronos tests/integration/pass/your_test.kr"
```

Run memory checks against multiple test files covering the new feature. If any memory issues are found:
1. Fix the issues
2. Return to Step 1 (re-plan the approach if the fix requires architectural changes)
3. Return to Step 2 (review twice)
4. Return to Step 3 (add/run tests) if needed
5. Re-run Steps 4-6 as needed

Do not proceed until valgrind reports zero errors and zero leaks.

### Step 7: REPL Support

Modify the REPL (in `main.c`) to support the new feature. After making changes, review your work **twice** as per Step 2. Fix any issues and re-run the twice review. Repeat as long as fixes are being made.

### Step 8: Documentation

Update the website documentation (`website/`) to reflect the changes made.

### Step 9: Compliance

Follow all steps above strictly. Do not skip steps or reduce the number of reviews.

## Code Style

- 4-space indentation, K&R braces
- Functions: `snake_case` (e.g., `value_new_number`)
- Types: `PascalCase` (e.g., `KronosValue`)
- Constants/Opcodes: `UPPER_SNAKE_CASE` (e.g., `OP_LOAD_CONST`)

## Testing

- **Integration tests**: `tests/integration/pass/` (should succeed) and `tests/integration/fail/` (expected errors)
- **Unit tests**: `tests/unit/` for tokenizer, parser, compiler, VM, runtime, GC
- Run a specific test: `./kronos tests/integration/pass/your_test.kr`
- Memory check with valgrind: `valgrind --leak-check=full ./kronos tests/integration/pass/your_test.kr`

## Commit Conventions

Format: `(Type): description`

Types: `Feat`, `Fix`, `Docs`, `Test`, `Refactor`, `Chore`

## Important Rules

Do not reference, cite, or link to any file that is not committed to git. Only reference tracked files that are expected to be committed.
