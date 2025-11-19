# Kronos Programming Language

A high-performance programming language built in C with human-readable syntax, featuring a bytecode VM, reference-counting garbage collection, and fast execution times.

## Features

- **Human-Readable Syntax**: Natural language keywords like `set`, `let`, `to`, `print`, `is equal`, `greater than`, etc.
- **Mutable & Immutable Variables**: Choose between `let` (mutable) and `set` (immutable)
- **Optional Type Annotations**: Enforce types with the `as` keyword
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

This runs 26 tests covering all implemented features. See [tests/README.md](tests/README.md) for details.

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

### Current Version (0.2.0)

✅ All core features working:

- Variables, numbers, strings
- Arithmetic and comparisons
- If statements, for/while loops
- **Functions with parameters and return values**
- **Local variable scoping**
- **Built-in Pi constant and math functions**
- REPL and file execution

### Future Versions

**v0.3.0 - Enhanced Control Flow**

- Else/else-if statements
- Break and continue
- Multiple return values

**v1.0.0 - Concurrency & Fault Tolerance**

- Goroutine-style lightweight threads
- Channel-based communication
- Exception handling (try/catch/finally)
- Supervisor trees (Erlang-style)

See [docs/PROJECT.md](docs/PROJECT.md) for detailed status and roadmap.

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
