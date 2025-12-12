# Contributing to Kronos

Thank you for your interest in contributing to Kronos! This document provides guidelines and instructions for contributing to the project.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Project Structure](#project-structure)
- [Development Workflow](#development-workflow)
- [Code Style](#code-style)
- [Testing](#testing)
- [Submitting Changes](#submitting-changes)
- [Areas for Contribution](#areas-for-contribution)
- [Documentation](#documentation)

## Code of Conduct

- Be respectful and inclusive
- Welcome newcomers and help them get started
- Focus on constructive feedback
- Be patient with questions and contributions

## Getting Started

### Prerequisites

- **C Compiler**: GCC or Clang (C11 standard)
- **Make**: Build system
- **Git**: Version control
- **Bash**: For running test scripts (Unix-like systems)

### Quick Start

1. **Fork and clone the repository:**

   ```bash
   git clone https://github.com/nednawr/kronos.git
   cd kronos
   ```

2. **Build the project:**

   ```bash
   make clean
   make
   ```

3. **Run the test suite:**

   ```bash
   ./scripts/run_tests.sh
   ```

4. **Try running an example:**
   ```bash
   ./kronos examples/hello.kr
   ```

## Development Setup

### Building

The project uses a Makefile for building:

```bash
# Build the main binary
make

# Build the LSP server
make lsp

# Clean build artifacts
make clean

# Build and run unit tests
make test-unit
```

### Build Configuration

The Makefile uses the following compiler flags:

- `-Wall -Wextra`: Enable all warnings
- `-std=c11`: C11 standard
- `-O2`: Optimization level 2
- `-g`: Include debug symbols
- `-Iinclude -Isrc`: Include paths

### Editor Setup

For the best development experience, install the VSCode extension:

```bash
./scripts/install_extension.sh
```

This provides:

- Syntax highlighting
- Real-time error checking
- Go-to-definition
- Hover information
- Autocomplete

See [docs/EDITOR.md](docs/EDITOR.md) for more details.

## Project Structure

```
kronos/
├── src/
│   ├── core/          # Runtime system (values, GC)
│   ├── frontend/      # Tokenizer and parser
│   ├── compiler/      # AST to bytecode compiler
│   ├── vm/            # Virtual machine
│   └── lsp/           # Language Server Protocol
├── include/           # Public headers
├── examples/          # Example .kr programs
├── tests/             # Test suite
│   ├── integration/   # Integration tests
│   └── unit/          # Unit tests
├── docs/              # Documentation
├── main.c             # Entry point
└── Makefile           # Build system
```

See [docs/PROJECT.md](docs/PROJECT.md) for detailed architecture documentation.

## Development Workflow

### 1. Create a Branch

```bash
git checkout -b feat/your-feature-name
# or
git checkout -b fix/your-bug-fix
```

Use descriptive branch names:

- `feat/` for new features
- `fix/` for bug fixes
- `docs/` for documentation
- `refactor/` for code refactoring

### 2. Make Your Changes

- Write clean, readable code
- Follow the code style guidelines
- Add tests for new features
- Update documentation as needed

### 3. Test Your Changes

```bash
# Run the full test suite
./run_tests.sh

# Run specific tests
./kronos tests/integration/pass/your_test.kr

# Run unit tests
make test-unit
```

**All tests must pass before submitting a pull request.**

### 4. Commit Your Changes

Write clear, descriptive commit messages following our [commit conventions](.github/COMMIT_CONVENTIONS.md):

```bash
git commit -m "(Feat): add support for modulo operator

- Add 'mod' keyword to tokenizer
- Implement modulo operation in compiler
- Add modulo bytecode instruction
- Update syntax documentation
- Add tests for modulo operator"
```

Commit message guidelines:

- Follow the format: `(Type): description`
- Use commit types: `(Feat)`, `(Fix)`, `(Docs)`, `(Test)`, `(Refactor)`, `(Chore)`
- Use the imperative mood ("add feature" not "added feature")
- First line should be a brief summary (50-72 characters recommended)
- Include a blank line after the first line
- Provide additional context in the body if needed
- Reference issues/PRs if applicable: `(#123)`

See [.github/COMMIT_CONVENTIONS.md](.github/COMMIT_CONVENTIONS.md) for detailed commit message guidelines and examples.

### 5. Push and Create a Pull Request

```bash
git push origin feat/your-feature-name
```

Then create a pull request on GitHub with:

- A clear title and description
- Reference to related issues
- Summary of changes
- Test results

## Code Style

### C Code Style

- **Indentation**: 4 spaces (no tabs)
- **Brace Style**: K&R style (opening brace on same line)
- **Naming**:
  - Functions: `snake_case` (e.g., `value_new_number`)
  - Types: `PascalCase` (e.g., `KronosValue`)
  - Constants: `UPPER_SNAKE_CASE` (e.g., `OP_LOAD_CONST`)
  - Variables: `snake_case` (e.g., `variable_name`)

Example:

```c
KronosValue* value_new_number(double num) {
    KronosValue* val = malloc(sizeof(KronosValue));
    if (val == NULL) {
        return NULL;
    }
    val->type = VAL_NUMBER;
    val->as.number = num;
    val->refcount = 1;
    return val;
}
```

### Code Quality Standards

- **Memory Safety**: No memory leaks (use reference counting)
- **Error Handling**: Check return values and handle errors gracefully
- **Comments**: Document complex logic and non-obvious behavior
- **Warnings**: Code should compile with zero warnings
- **Modularity**: Keep functions focused and modules independent

### Kronos Code Style

For `.kr` example and test files:

- Use descriptive variable names
- Include comments explaining the purpose
- Follow the language's natural syntax style
- Keep examples focused and clear

## Testing

### Test Structure

The test suite includes:

1. **Unit Tests** (`tests/unit/`): Test individual components

   - Tokenizer tests
   - Parser tests
   - Compiler tests
   - VM tests
   - Runtime tests
   - GC tests

2. **Integration Tests** (`tests/integration/`):
   - **Passing tests** (`pass/`): Should execute successfully
   - **Error tests** (`fail/`): Should produce expected errors

### Running Tests

```bash
# Run all tests
./run_tests.sh

# Run unit tests only
make test-unit

# Run a specific integration test
./kronos tests/integration/pass/variables_immutable.kr

# Run all passing tests
for f in tests/integration/pass/*.kr; do ./kronos "$f"; done
```

### Adding Tests

**For new features:**

1. Add a passing test in `tests/integration/pass/`
2. Add error tests in `tests/integration/fail/` if applicable
3. Add unit tests in `tests/unit/` for low-level functionality

**Test file naming:**

- Use descriptive names: `variables_immutable.kr`
- Include a comment describing what the test validates

Example:

```kronos
# Test: Modulo operator with positive numbers
# Expected: Pass

set result to 10 mod 3
print result  # Should print 1
```

**For bug fixes:**

1. Add a test that reproduces the bug
2. Verify the test fails before your fix
3. Verify the test passes after your fix

### Test Requirements

- All new features must include tests
- Bug fixes must include regression tests
- Tests should be clear and self-documenting
- Error tests should verify the correct error message

## Submitting Changes

### Pull Request Process

1. **Update your branch:**

   ```bash
   git checkout develop
   git pull origin develop
   git checkout your-branch
   git rebase develop
   ```

2. **Ensure tests pass:**

   ```bash
   ./scripts/run_tests.sh
   ```

3. **Memory leak checks** (automatic in CI):

   Memory leak detection runs automatically in CI via `.github/workflows/memory-check.yml`.
   For local testing with valgrind, you can manually run:

   ```bash
   # If you have valgrind installed
   valgrind --leak-check=full --show-leak-kinds=all ./kronos tests/integration/pass/your_test.kr
   ```

4. **Create the pull request:**
   - Target the `develop` branch
   - Provide a clear description
   - Reference related issues
   - Include test results

### Pull Request Checklist

- [ ] Code follows the style guidelines
- [ ] All tests pass (`./run_tests.sh`)
- [ ] New features include tests
- [ ] Documentation is updated
- [ ] Commit messages are clear
- [ ] No compiler warnings
- [ ] Code is ready for production (no half-baked features)

### Review Process

- Maintainers will review your PR
- Address feedback promptly
- Be open to suggestions and improvements
- Keep discussions constructive

## Areas for Contribution

### High Priority

- **File I/O Operations**: Complete the file system interface (see [ROADMAP.md](ROADMAP.md))
- **Path Operations**: File path utilities
- **Regular Expressions**: Pattern matching support
- **LSP Improvements**: Enhanced language server features

### Testing

- Add more integration tests
- Improve test coverage
- Add performance benchmarks
- Add edge case tests

### Documentation

- Improve code comments
- Add more examples
- Expand syntax documentation
- Write tutorials

### Bug Fixes

- Check the issue tracker for reported bugs
- Fix memory leaks or performance issues
- Improve error messages
- Fix edge cases

### Code Quality

- Refactor complex code
- Improve error handling
- Optimize hot paths
- Reduce code duplication

### Features from Roadmap

See [ROADMAP.md](ROADMAP.md) for planned features. Pick something that interests you!

## Documentation

### Code Documentation

- Header files should document public APIs
- Complex functions should have inline comments
- Explain non-obvious algorithms or optimizations

### User Documentation

- Update [docs/SYNTAX.md](docs/SYNTAX.md) for language changes
- Update [docs/QUICKREF.md](docs/QUICKREF.md) for quick reference
- Add examples to `examples/` directory
- Update [README.md](README.md) if needed

### Developer Documentation

- Update [docs/PROJECT.md](docs/PROJECT.md) for architecture changes
- Document new components and modules
- Update this file if workflow changes

## Getting Help

- **Documentation**: Check [docs/](docs/) for detailed information
- **Issues**: Search existing issues on GitHub
- **Questions**: Open a discussion or issue on GitHub

## Recognition

Contributors will be recognized in:

- Git commit history
- Release notes
- Project documentation (future)

Thank you for contributing to Kronos! 🎉
