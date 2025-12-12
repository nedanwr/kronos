# Kronos Roadmap

This document outlines the planned features and release schedule for Kronos.

## Current Status

**Current Version:** 0.4.0
**Status:** ✅ Stable - Core language features complete

### What's Available Now

- ✅ Core language features (variables, types, operators, control flow)
- ✅ Functions with parameters and return values
- ✅ Logical operators (`and`, `or`, `not`)
- ✅ Lists/arrays with full operations (indexing, slicing, iteration)
- ✅ Maps/dictionaries with hash table implementation (literals, indexing, key-value storage)
- ✅ String operations (concatenation, indexing, slicing, built-ins)
- ✅ F-strings (string interpolation)
- ✅ Enhanced standard library (math functions, type conversion, list utilities)
- ✅ Module system (built-in modules like `math`)
- ✅ Enhanced control flow (else-if, break, continue)
- ✅ LSP support (error checking, go-to-definition, hover, completions)
- ✅ Range objects (literals, indexing, slicing, iteration, length)

---

## Upcoming Releases

### Version 0.4.0: "Modules & Error Handling"

**Target:** Q4 2025
**Status:** 🚧 In Progress

#### Planned Features

- ✅ **Range Objects** - First-class range support (completed)

  - Range literals: `range 1 to 10`, `range 0 to 100 by 5`
  - Range indexing: `r at 0`, `r at -1` (supports positive and negative indices)
  - Range length: `call len with r`
  - Range iteration: `for i in r:`
  - Range slicing: `r from 2 to 5`

- ✅ **Type Conversion Functions** - Complete type conversion utilities (completed)

  - `to_number()` - ✅ Convert string to number
  - `to_bool()` - ✅ Convert string/number to boolean
  - `to_string()` - ✅ Already implemented

- ✅ **Dictionaries/Maps** - Key-value storage with hash table implementation (completed)

  - Map literals: `map key: value, key2: value2`
  - Map indexing: `map at key` (supports string, number, boolean, and null keys)
  - Map operations: `map_get()`, `map_set()`, `map_delete()`
  - Hash table implementation with automatic growth
  - Memory management with reference counting and garbage collection
  - Map printing and equality comparison

  ```kronos
  set person to map name: "Alice", age: 30, city: "NYC"
  print person at "name"
  # Note: Map assignment (let person at "age" to 31) deferred to future
  ```

- ✅ **File-based Module System** - Import code from other `.kr` files (completed)

  ```kronos
  import utils from "mylib.kr"
  # Modules are executed when imported
  # Functions and variables from modules are accessible via module namespace
  ```

- ✅ **Core Operator Improvements** - Missing fundamental operators (completed)

  - ✅ Modulo operator (`mod`): `set remainder to 10 mod 3` → `1`
  - ✅ Unary negation (`-x`): `set neg to -value` instead of `0 minus value`

- ✅ **Mutable Collection Operations** - In-place modification of collections (completed)

  - ✅ List index assignment: `let nums at 0 to 10` (modify list element)
  - ✅ Map key deletion: `delete person at "age"` (remove key from map)

  ```kronos
  let nums to list 1, 2, 3
  let nums at 0 to 10      # nums is now [10, 2, 3]

  let person to map name: "Alice", age: 30
  delete person at "age"   # person is now {name: "Alice"}
  ```

- ✅ **Exception Handling** - Try/catch/finally blocks for better error management (completed)

  ```kronos
  try:
      raise ValueError "Invalid input"
  catch ValueError as e:
      print "Caught ValueError:", e
  catch error:
      print "Caught error:", error
  finally:
      print "Cleanup"
  ```

- ✅ **File I/O Operations** - Complete file system interface (completed)

  ```kronos
  set content to call read_file with "data.txt"
  call write_file with "output.txt", content
  set lines to call read_lines with "data.txt"
  set exists to call file_exists with "data.txt"
  set files to call list_files with "."
  ```

- ✅ **Path Operations** - File path utilities (completed)

  ```kronos
  set full_path to call join_path with "dir", "file.txt"
  set dir to call dirname with "/path/to/file.txt"
  set file to call basename with "/path/to/file.txt"
  ```

- ✅ **Regular Expressions** - Pattern matching and text processing (completed)

  ```kronos
  import regex
  set matches to call regex.match with "hello", "h.*o"
  set result to call regex.search with "hello world", "world"
  set all_matches to call regex.findall with "cat, bat, sat", "[a-z]at"
  ```

- ✅ **LSP Improvements** (Completed)

  - ✅ Hover info for file-based modules (show module path and exports)
  - ✅ Module function validation (verify functions exist in imported modules)
  - ✅ Find all references
  - ✅ Rename symbol
  - ✅ Code actions & quick fixes
  - ✅ Document formatting
  - ✅ Workspace symbols
  - ✅ Code lens

- ✅ **REPL Expression Statements** - Python-like interactive shell (completed)

  - ✅ Allow expressions to be used as statements in the REPL
  - ✅ Automatically print expression results (like Python's interactive shell)
  - ✅ Support for evaluating expressions like `10 plus 20` directly in the REPL
  - ✅ Example: Running `./kronos` and typing `42` or `10 plus 20` will print the result
  - ✅ Command-line execution flag: `./kronos -e "print 42"` to execute code without entering REPL
  - ✅ Support multiple `-e` flags: `./kronos -e "set x to 10" -e "print x"`

- ✅ **REPL Line Editing & History** - Enhanced interactive experience (completed)

  - ✅ Arrow key navigation (up/down for history, left/right for editing)
  - ✅ Command history (configurable size: 100 entries, persistent across sessions via `.kronos_history`)
  - ✅ Tab completion (keywords, function names, variable names)
  - ✅ Basic editing (backspace, delete, home/end keys)
  - ✅ Implemented using **linenoise**: Lightweight, single-file library, BSD license, no external dependencies
  - History is automatically saved to `.kronos_history` file in the current directory
  - Tab completion provides suggestions for all Kronos keywords, user-defined functions, and global variables

- 📝 **Code Documentation Improvements** - Improve comment quality (in progress)

  - Improve comments to explain "why" rather than just "what"
  - Add design decision documentation
  - Document edge cases and non-obvious behavior
  - Incremental improvement as code is modified

- 📋 **Code Refactoring** - Improve code organization (future)
  - Refactor functions with long parameter lists (5+ parameters) to use parameter structs
  - Example: `call_module_function()` currently takes 5 parameters; consider using a struct for better maintainability

---

### Version 0.5.0: "Advanced Language Features"

**Target:** N/A
**Status:** 📋 Planned

#### Planned Features

- **String Interpolation Enhancements** - Advanced formatting features

  ```kronos
  set price to 19.99
  set formatted to f"Price: {price:.2f}"  # Format specifiers
  ```

- **Multiple Return Values** - Tuple returns and destructuring

  ```kronos
  function divide_with_remainder with a, b:
      return q, r

  set q, r to call divide_with_remainder with 10, 3
  set x, y to y, x  # Swap
  ```

- **Function Enhancements**

  - Default parameter values: `function greet with name, greeting="Hello":`
  - Variadic functions: `function sum with ...numbers:`
  - Named arguments: `call create_user with name: "Alice", age: 30`

- **Anonymous Functions / Lambdas** - First-class functions

  ```kronos
  set double to function with x: return x times 2
  set doubled to map numbers with function with x: return x times 2
  ```

- **List Utilities (Higher-Order)** - `filter()` and `map()` functions

  ```kronos
  set evens to filter numbers with function with x: return (x divided by 2) times 2 is equal x
  set doubled to map numbers with function with x: return x times 2
  ```

- **List Comprehensions** - Concise list creation

  ```kronos
  set squares to [x times x for x in range 1 to 10]
  set evens to [x for x in range 1 to 20 if (x divided by 2) times 2 is equal x]
  ```

- **Pattern Matching** - Advanced control flow

  ```kronos
  match value:
      case 1:
          print "One"
      case 2:
          print "Two"
      default:
          print "Other"
  ```

- **Type System Enhancements**

  - Generic types: `list<number>`, `map<string, number>`
  - Type aliases: `type Point to map x: number, y: number`
  - Multi-type support: `as number or string`

- **Debugging Support**

  ```kronos
  debug "Variable x:", x
  ```

- **LSP Improvements**
  - Signature help
  - Semantic tokens
  - Inlay hints
  - Call hierarchy
  - Code folding
  - Bracket pair colorization

---

### Version 1.0.0: "Production Release"

**Target:** N/A
**Status:** 📋 Planned

#### Planned Features

- **Concurrency** - Goroutines and channels (Go-inspired)

  ```kronos
  spawn task with:
      for i in range 1 to 10:
          print "Task:", i

  set ch to channel
  spawn sender with ch:
      send ch, "hello"
  set msg to receive ch
  ```

- **Complete Standard Library** - 50+ functions across multiple modules

  - **Math:** `sin()`, `cos()`, `tan()`, `asin()`, `acos()`, `atan()`, `log()`, `log10()`, `exp()`, `cbrt()`
  - **String:** `find()`, `rfind()`, `count()`, `capitalize()`, `title()`
  - **Date/Time:** `now()`, `format_date()`, `parse_date()`, `sleep()`
  - **Collections:** `zip()`, `enumerate()`, `any()`, `all()`, `sum()`
  - **System:** `exit()`, `args()`, `env()`
  - **JSON:** `parse_json()`, `to_json()`
  - **CSV:** `read_csv()`, `write_csv()`, `parse_csv()`, `to_csv()`
  - **Test Framework:** `assert`, `test`, `run_tests`
  - **Crypto:** `md5()`, `sha1()`, `sha256()`, `sha512()`, `random_bytes()`, `secure_random_int()`
  - **HTTP:** `http_get()`, `http_post()`, HTTP server (requires concurrency)
  - **CLI:** `parse_args()`, argument parsing utilities

- **Method Chaining** - Fluent API support

  ```kronos
  set result to text.uppercase().trim().split(" ")
  set processed to list 1, 2, 3.filter(function with x: return x is greater than 2).map(function with x: return x times 2)
  ```

- **Performance Optimizations**

  - Bytecode optimization passes
  - Constant folding
  - Dead code elimination
  - Inline caching for method calls
  - Profile-guided optimization
  - **F-string Expression Parsing Optimization** - Parse embedded expressions inline
    - Currently f-strings re-tokenize embedded expressions, which is inefficient
    - Optimize by tracking source positions in tokens and parsing inline
    - Requires architectural changes: position tracking, substring parsing capability
    - Will eliminate redundant tokenization for f-string expressions

- **Standard Library Modules**

  - `math` - Complete mathematical functions
  - `string` - String utilities
  - `os` - Operating system interface
  - `json` - JSON parsing and generation
  - `csv` - CSV parsing and generation
  - `test` - Testing framework
  - `crypto` - Cryptographic functions
  - `http` - HTTP client and server
  - `cli` - Command-line argument parsing
  - `time` - Time and date operations
  - `collections` - Collection utilities
  - `regex` - Regular expressions

- **Package Manager** - Install and manage packages

  ```bash
  kronos install <package>
  kronos list
  kronos remove <package>
  ```

- **Code Formatter** - Opinionated formatter (like `gofmt`/`rustfmt`)

  ```bash
  kronos format <file>
  kronos format --check <file>
  ```

- **Linter** - Code quality checks

  ```bash
  kronos lint <file>
  kronos lint --fix <file>
  ```

- **Test Runner** - Built-in testing framework

  ```bash
  kronos test
  kronos test --verbose
  ```

- **Documentation Generator** - Generate docs from code

  ```bash
  kronos doc
  kronos doc --output docs/
  ```

- **LSP Complete** - Full language server protocol support
  - All previous LSP features
  - Workspace symbols
  - Call hierarchy
  - Multi-root workspace support
  - Production-ready performance

---

## Post-1.0.0 (Future)

### Advanced Features

- **Supervisor Trees / Fault Tolerance** (Erlang-style)

  - Process supervision
  - Restart strategies
  - Fault isolation

- **Package Manager (Advanced)**

  - Version resolution
  - Dependency graph
  - Lock files (`kronos.lock`)
  - Package caching
  - Offline mode support
  - Parallel package installation
  - Dependency conflict resolution

- **First-Party Installable Packages**

  - `@kronos/yaml` - YAML parsing and generation
  - `@kronos/toml` - TOML parsing and generation
  - `@kronos/xml` - XML parsing and generation
  - `@kronos/websocket` - WebSocket client and server
  - `@kronos/math-advanced` - Advanced mathematical functions
  - `@kronos/collections-advanced` - Advanced data structures
  - `@kronos/debug` - Enhanced debugging utilities

- **Web Templating Engine** (Blade-like)

  - Template inheritance
  - Component system
  - HTML escaping
  - Control flow directives

- **Performance Optimizations**

  - JIT compilation
  - Hot path optimization
  - Profile-guided optimization
  - **Dynamic String Intern Table** - Configurable and growable intern table
    - Replace fixed-size hash table with dynamically allocated table
    - Support for configuration via environment variable or API
    - Automatic growth when load factor exceeds threshold
    - Rehashing logic for table expansion
    - Improved memory efficiency for programs with many unique strings
  - **Relative Epsilon Comparison** - Improved floating-point comparison accuracy
    - Replace fixed epsilon with magnitude-scaled relative epsilon
    - More accurate comparisons for very large numbers (e.g., 1e20)
    - More accurate comparisons for very small numbers
    - Better handling of floating-point precision across different numeric ranges

- **Advanced Type System**
  - Optional static typing
  - Type inference
  - Generic types
  - Type constraints

---

## Feature Requests & Feedback

Have a feature idea? We'd love to hear from you! Please open an issue on GitHub to discuss.

## Contributing

Interested in contributing? Check out our [contributing guidelines](CONTRIBUTING.md) (coming soon) or review the [development documentation](docs/PROJECT.md).

---

**Last Updated:** November 2025
