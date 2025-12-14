# LSP Test Suite

Tests for the Kronos Language Server Protocol (LSP) implementation.

## Running Tests

```bash
make test-lsp
```

This will:
1. Build the LSP server (`kronos-lsp`) if needed
2. Build the LSP test suite
3. Run all LSP feature tests

## Test Coverage

The test suite covers all new LSP features:

1. **Hover Info for File-Based Modules** - Tests hover information display for imported modules
2. **Module Function Validation** - Tests validation of module function calls
3. **Find All References** - Tests finding all usages of a symbol
4. **Rename Symbol** - Tests renaming symbols across all references
5. **Code Actions** - Tests code action placeholder (returns empty array)
6. **Document Formatting** - Tests document formatting functionality
7. **Workspace Symbols** - Tests workspace-wide symbol search
8. **Code Lens** - Tests code lens display (reference counts, parameter info)

## Test Framework

The test framework (`test_lsp_framework.c/h`) provides:
- LSP server process management (fork/exec)
- JSON-RPC message sending and receiving
- Helper functions for common LSP requests
- Response parsing and validation utilities

## Test Structure

- `test_lsp_main.c` - Main test runner
- `test_lsp_framework.c/h` - LSP test framework utilities
- `test_lsp_features.c` - Individual test cases for each feature

## Notes

- Tests require the LSP server to be built (`make lsp`)
- Tests spawn the LSP server as a subprocess and communicate via pipes
- Each test sets up a document, sends LSP requests, and validates responses

