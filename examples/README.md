# Kronos Examples

Example programs demonstrating Kronos language features.

## Quick Start

Run any example:
```bash
./kronos examples/hello.kr
```

## Examples by Category

### Basic
- **hello.kr** - Hello world and basic printing
- **variables.kr** - Mutable/immutable variables, types, booleans, null
- **arithmetic.kr** - All arithmetic operations

### Control Flow
- **conditionals.kr** - If statements and comparisons
- **loops.kr** - For and while loops
- **control_flow.kr** - Else-if, break, continue, range steps, imports

### Functions
- **functions_simple.kr** - Basic function definitions and calls
- **functions.kr** - Comprehensive function examples with scoping

### Data Structures
- **lists.kr** - List literals, indexing, slicing, and iteration
- **maps.kr** - Map literals, key-value access, and different key types
- **range_objects.kr** - Range objects with indexing and iteration

### File Operations
- **file_operations.kr** - File I/O (read_file, write_file, read_lines, file_exists, list_files) and path operations (join_path, dirname, basename)

### Regular Expressions
- **regex_usage.kr** - Pattern matching with regex module (match, search, findall)

### Advanced
- **builtins.kr** - Built-in constants (Pi) and math functions
- **syntax_showcase.kr** - Complete feature demonstration
- **fizzbuzz.kr** - FizzBuzz implementation
- **file_operations.kr** - File I/O and path operations

## Learning Path

1. **hello.kr** → **variables.kr** → **arithmetic.kr**
2. **conditionals.kr** → **loops.kr** → **control_flow.kr**
3. **functions_simple.kr** → **functions.kr**
4. **lists.kr** → **maps.kr** → **range_objects.kr**
5. **file_operations.kr** - File I/O and path operations
6. **regex_usage.kr** - Regular expressions and pattern matching
7. **builtins.kr** → **syntax_showcase.kr** → **fizzbuzz.kr**

## Running All Examples

```bash
for file in examples/*.kr; do
    echo "=== Running $file ==="
    ./kronos "$file"
    echo ""
done
```

## Documentation

- [Complete Syntax Reference](../docs/SYNTAX.md)
- [Quick Reference](../docs/QUICKREF.md)
- [Main README](../README.md)
