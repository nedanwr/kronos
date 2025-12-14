# Kronos Programming Language

A high-performance programming language built in C with human-readable syntax, featuring a bytecode VM, reference-counting garbage collection, and fast execution times.

## Features

### Language Features

- **Human-Readable Syntax**: Natural language keywords like `set`, `let`, `to`, `print`, `is equal`, `greater than`, etc.
- **Mutable & Immutable Variables**: Choose between `let` (mutable) and `set` (immutable)
- **Optional Type Annotations**: Enforce types with the `as` keyword
- **F-Strings**: Formatted string literals with expression interpolation (`f"Hello, {name}!"`)
- **String Operations**: Concatenation, indexing, slicing, and comprehensive built-in functions
- **Lists & Arrays**: List literals, indexing, slicing, and iteration
- **Maps/Dictionaries**: Key-value storage with hash table implementation, map literals, and indexing
- **Range Objects**: First-class range support with indexing, slicing, and iteration
- **Enhanced Standard Library**: Math functions (sqrt, power, abs, round, floor, ceil, rand, min, max), type conversion (to_number, to_bool), and list utilities (reverse, sort)
- **Module System**: Import built-in modules (`import math`) and file-based modules (`import utils from "utils.kr"`). Use namespaced functions (`math.sqrt`, `utils.function`). String functions are global built-ins.
- **Control Flow**: If/else-if/else, for/while loops, break/continue statements
- **Functions**: First-class functions with parameters, return values, and local scoping

### Runtime & Performance

- **Fast Execution**: Bytecode VM with optimized execution (Python/JS performance levels)
- **Reference Counting GC**: Automatic memory management with cycle detection
- **Direct Execution**: No build step required - just run `.kr` files directly
- **Interactive REPL**: Test code snippets interactively
- **Small Binary**: ~58KB compiled binary, ~15ms startup time

### Developer Experience

- **Editor Support**: Syntax highlighting for VSCode, Vim, Sublime, and more
- **LSP Support**: Real-time error checking, go-to-definition, hover information, and autocomplete
- **Comprehensive Testing**: 129 tests (83 passing, 46 expected fail cases) ensuring reliability

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
./scripts/run_tests.sh
```

This runs 129 tests (83 passing, 46 expected fail cases) covering all implemented features. See [tests/README.md](tests/README.md) for details.

### Editor Setup (Optional)

Get full IDE features (syntax highlighting, error checking, autocomplete):

```bash
./scripts/install_extension.sh
```

Then restart VSCode/Cursor/Windsurf. See [EDITOR.md](docs/EDITOR.md) for details.

## Language Syntax

Kronos uses human-readable syntax with natural language keywords. Here's a comprehensive example:

```kronos
# Variables
set x to 10                    # Immutable
let counter to 0               # Mutable
let counter to counter plus 1  # Can reassign

# Type annotations
let age to 25 as number
set name to "Alice" as string

# Data types
set isActive to true
set result to null
set numbers to list 1, 2, 3
set range_obj to range 1 to 10

# Strings and f-strings
set greeting to "Hello"
set message to f"Hello, {name}!"
set first_char to greeting at 0
set slice to greeting from 0 to 3

# Functions
function greet with name:
    print f"Hello, {name}!"
    return name

call greet with "World"

# Control flow
if x is greater than 5:
    print "Large"
else if x is equal 5:
    print "Medium"
else:
    print "Small"

# Loops
for i in range 1 to 5:
    print i

for item in numbers:
    print item

while counter is less than 10:
    print counter
    let counter to counter plus 1
    if counter is equal 5:
        break

# Lists and ranges
set my_list to list 1, 2, 3, 4, 5
print my_list at 0           # First element
print my_list at -1          # Last element
print my_list from 1 to 3    # Slice

set r to range 0 to 20 by 5
print r at 2                 # Index into range
print call len with r        # Range length

# Maps
set person to map name: "Alice", age: 30
print person at "name"       # "Alice"
print person at "age"        # 30
for i in r:                  # Iterate range
    print i
```

See [docs/SYNTAX.md](docs/SYNTAX.md) for the complete syntax reference or [docs/QUICKREF.md](docs/QUICKREF.md) for a quick reference card.

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

## Contributing

Contributions are welcome! See [.github/CONTRIBUTING.md](.github/CONTRIBUTING.md) for guidelines. The codebase is well-documented and modular. See [docs/PROJECT.md](docs/PROJECT.md) for architecture details.

## License

MIT License - See [LICENSE](LICENSE) file for details.

## Third-Party Libraries

Kronos uses the following third-party libraries:

- **linenoise** - Line editing library for the REPL
  - Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
  - Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
  - BSD License
  - Source: https://github.com/antirez/linenoise
  - Used for: REPL line editing, command history, and tab completion

## Performance

- **Binary Size**: 58KB
- **Startup Time**: ~15ms
- **Execution**: Comparable to CPython
- **Memory**: Zero leaks in core features

Built with C for high performance and low-level control.
