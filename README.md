# Kronos Programming Language

A high-performance programming language built in C with human-readable syntax, featuring a bytecode VM, reference-counting garbage collection, and fast execution times.

## Features

- **Human-Readable Syntax**: Natural language keywords like `set`, `let`, `to`, `print`, `is equal`, `greater than`, etc.
- **Mutable & Immutable Variables**: Choose between `let` (mutable) and `set` (immutable)
- **Optional Type Annotations**: Enforce types with the `as` keyword
- **F-Strings**: Formatted string literals with expression interpolation (`f"Hello, {name}!"`)
- **String Operations**: Concatenation, indexing, slicing, and comprehensive built-in functions
- **Lists & Arrays**: List literals, indexing, slicing, and iteration
- **Enhanced Standard Library**: Math functions (sqrt, power, abs, round, floor, ceil, rand, min, max), type conversion (to_number, to_bool), and list utilities (reverse, sort)
- **Module System**: Import built-in modules (`import math`) and use namespaced functions (`math.sqrt`). String functions are global built-ins.
- **Fast Execution**: Bytecode VM with optimized execution (Python/JS performance levels)
- **Reference Counting GC**: Automatic memory management with cycle detection
- **Direct Execution**: No build step required - just run `.kr` files directly
- **Interactive REPL**: Test code snippets interactively
- **Editor Support**: Syntax highlighting for VSCode, Vim, Sublime, and more
- **LSP Support**: Real-time error checking (in development)

## Quick Start

### Building

```bash
make
```

### Running

Execute a Kronos file:

```bash
./kronos examples/hello.kr
```

Start the REPL:

```bash
./kronos
```

### Testing

Run the comprehensive test suite:

```bash
./run_tests.sh
```

This runs 27 tests covering all implemented features. See [tests/README.md](tests/README.md) for details.

### Editor Setup (Optional)

Get full IDE features (syntax highlighting, error checking, autocomplete):

```bash
./install_extension.sh
```

Then restart VSCode/Cursor/Windsurf. See [EDITOR.md](docs/EDITOR.md) for details.

## Language Syntax

Kronos uses human-readable syntax with natural language keywords. Here's a quick example:

```kronos
# Immutable variable
set x to 10

# Mutable variable
let counter to 0
let counter to counter plus 1

# Type-annotated variables
let age to 25 as number
set name to "Alice" as string

# Booleans and null
set isActive to true
set result to null

# Functions
function greet with name:
    print "Hello,"
    print name
    return name

call greet with "World"

# Loops
for i in range 1 to 5:
    print i
```

## Documentation

- 📖 **[SYNTAX.md](docs/SYNTAX.md)** - Complete language reference
- ⚡ **[QUICKREF.md](docs/QUICKREF.md)** - Quick reference card
- 🏗️ **[PROJECT.md](docs/PROJECT.md)** - Architecture & implementation
- 🎨 **[EDITOR.md](docs/EDITOR.md)** - Editor setup & LSP
- 📦 **[All Docs](docs/README.md)** - Documentation index

## Examples

All examples are in the `examples/` directory:

| File                  | Description                      |
| --------------------- | -------------------------------- |
| `hello.kr`            | Hello world and basic printing   |
| `test.kr`             | Variables and arithmetic         |
| `arithmetic.kr`       | All arithmetic operations        |
| `conditionals.kr`     | If statements                    |
| `loops.kr`            | For and while loops              |
| `fizzbuzz.kr`         | FizzBuzz implementation          |
| `syntax_showcase.kr`  | Feature demonstration            |
| `functions_simple.kr` | Simple function examples         |
| `functions.kr`        | Comprehensive function demos     |
| `builtins.kr`         | Built-in constants and functions |
| `variables.kr`        | Variable mutability and types    |
| `pi_constant.kr`      | Pi constant usage                |

**Run an example:**

```bash
./kronos examples/hello.kr
```

See [examples/README.md](examples/README.md) for more details.

## Architecture

### Components

- **Frontend** (`src/frontend/`) - Tokenizer and parser
- **Compiler** (`src/compiler/`) - AST to bytecode compilation
- **Virtual Machine** (`src/vm/`) - Stack-based bytecode execution
- **Core Runtime** (`src/core/`) - Value system and garbage collector

### Bytecode VM

Stack-based virtual machine with instructions for:

- Variable operations (`LOAD_VAR`, `STORE_VAR`)
- Arithmetic (`ADD`, `SUB`, `MUL`, `DIV`)
- Comparisons (`EQ`, `NEQ`, `GT`, `LT`, `GTE`, `LTE`)
- Control flow (`JUMP`, `JUMP_IF_FALSE`)
- I/O (`PRINT`)

See [docs/PROJECT.md](docs/PROJECT.md) for detailed architecture documentation.

## Development

### Project Structure

```
kronos/
├── src/
│   ├── core/          # Core runtime (values, memory, GC)
│   ├── frontend/      # Tokenizer and parser
│   ├── compiler/      # Bytecode compiler
│   ├── vm/            # Virtual machine
│   ├── concurrency/   # Future: Goroutines & channels
│   └── fault/         # Future: Exceptions & supervisors
├── include/           # Public headers
├── examples/          # Example .kr files
├── docs/              # Documentation
├── main.c             # Entry point
└── Makefile
```

### Building for Development

```bash
make clean
make
```

### Cleaning Build Artifacts

```bash
make clean
```

## Roadmap

See [ROADMAP.md](ROADMAP.md) for the complete roadmap and upcoming features.

### Current Version (0.3.0)

✅ **Completed:**
- Core language features (variables, types, operators, control flow)
- Functions with parameters and return values
- Built-in constants and functions
- Logical operators (`and`, `or`, `not`)
- Lists/arrays with full operations (indexing, slicing, iteration)
- String operations (concatenation, indexing, slicing, built-ins)
- F-strings (string interpolation)
- Enhanced standard library and module system
- Enhanced control flow (else-if, break, continue)
- LSP support (error checking, go-to-definition, hover, completions)
- Range objects (literals, indexing, slicing, iteration, length)
- REPL and file execution

### Upcoming (v0.4.0 - Q2 2025)

- Type conversion functions (`to_number()`, `to_bool()`)
- Dictionaries/maps
- File-based module system
- Exception handling
- File I/O operations
- Regular expressions

**v0.5.0: "Advanced Language Features" (Q3 2025)**

- String interpolation - Template strings with expressions and format specifiers
- Multiple return values - Tuple returns and destructuring
- Function enhancements - Default parameters, variadic functions, named arguments
- Anonymous functions / Lambdas - First-class functions, higher-order functions
- List comprehensions - Concise list creation with conditionals
- Pattern matching - Advanced control flow with match expressions
- Type system enhancements - Generic types, type aliases, better inference
- Debugging support - Debug built-in, improved stack traces
- Enhanced LSP - Signature help, semantic tokens, inlay hints, call hierarchy

**v1.0.0: "Production Release" (Q4 2025)**

- Concurrency - Goroutines and channels with `spawn`, `send`, `receive`, `select`, worker pools
- Complete standard library - 50+ functions (math, string, date/time, collections, JSON, system)
- Method chaining - Fluent API support
- Performance optimizations - Bytecode optimization, constant folding, inline caching
- Standard library modules - `math`, `string`, `os`, `json`, `time`, `collections`, `regex`
- Production-ready LSP - All features, multi-root workspace support
- Tooling - Package manager, formatter, linter, test runner, documentation generator

**Release Criteria:**
- All core language features implemented
- Complete standard library (50+ functions)
- Production-ready LSP with all major features
- Comprehensive test coverage (150+ tests)
- Full documentation (user guide, API reference, tutorials)
- Performance benchmarks met (startup < 20ms, execution competitive)
- Memory safety verified (valgrind clean, no leaks)
- Cross-platform support (Linux, macOS, Windows)
- CI/CD pipeline with automated testing

See [ROADMAP.md](ROADMAP.md) for the roadmap and upcoming features, or [docs/PROJECT.md](docs/PROJECT.md) for architecture details.

## Contributing

Contributions are welcome! The codebase is well-documented and modular. See [docs/PROJECT.md](docs/PROJECT.md) for architecture details.

## License

MIT License - See [LICENSE](LICENSE) file for details.

## Performance

- **Binary Size**: 58KB
- **Startup Time**: ~15ms
- **Execution**: Comparable to CPython
- **Memory**: Zero leaks in core features

Built with C for high performance and low-level control.
