# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

_No unreleased changes._

## [0.4.5] - 2026-01-05

### Added

- **Official Website** - Modern documentation site at kronos.nedanwr.dev with full language reference, tutorials, and guides
- **Interactive Playground** - Browser-based code editor with WebAssembly-powered execution, syntax highlighting, and real-time output
- **WebAssembly Support** - Kronos interpreter compiled to WASM via Emscripten for client-side code execution
- **Compile-time Warnings** - Division/modulo by zero detection with warning messages
- **Syntax Highlighting** - TextMate grammar-based highlighting for Kronos code

### Changed

- **Documentation Structure** - Migrated detailed docs from markdown files to website; repository now contains only essential project docs
- **Compiler Diagnostics** - Enhanced compiler to support warning callbacks alongside errors

### Fixed

- **Compiler Control Flow** - Fixed `else if` jump emission order that caused incorrect branch execution
- **VM State Reset** - Added proper state clearing between WASM executions

## [0.4.0] - 2025-12-XX

### Added

- **Range Objects** - First-class range support with literals, indexing, slicing, iteration, and length operations
- **Type Conversion Functions** - Complete type conversion utilities (`to_number()`, `to_bool()`, `to_string()`)
- **Dictionaries/Maps** - Key-value storage with hash table implementation, map literals, indexing, and operations
- **File-based Module System** - Import code from other `.kr` files with namespace support
- **Core Operator Improvements** - Modulo operator (`mod`) and unary negation (`-x`)
- **Mutable Collection Operations** - List index assignment (`let nums at 0 to 10`) and map key deletion (`delete person at "age"`)
- **Exception Handling** - Try/catch/finally blocks with typed exceptions and error propagation
- **File I/O Operations** - Complete file system interface (`read_file`, `write_file`, `read_lines`, `file_exists`, `list_files`)
- **Path Operations** - File path utilities (`join_path`, `dirname`, `basename`)
- **Regular Expressions** - Pattern matching and text processing via `regex` module
- **LSP Improvements** - Hover info for file-based modules, module function validation, find all references, rename symbol, code actions & quick fixes, document formatting, workspace symbols, and code lens
- **REPL Expression Statements** - Python-like interactive shell with automatic expression result printing
- **REPL Line Editing & History** - Arrow key navigation, command history (100 entries, persistent via `.kronos_history`), tab completion, and basic editing using linenoise library
- **Command-line Execution Flag** - `-e` option for executing Kronos code directly from command line, supporting multiple `-e` flags
- **Code Documentation Improvements** - Enhanced comments explaining design decisions, edge cases, and non-obvious behavior
- **Docker Support** - Dockerfile for lightweight development environment with Valgrind support
- **Valgrind Testing Script** - Automated memory leak detection for integration tests
- **Example Scripts** - New examples for exception handling, type conversion, module usage, data processing, and configuration reading
- **Integration Tests** - Comprehensive edge case tests for type conversion, file I/O, list functions, math functions, and string functions

### Changed

- **VM Global Variable Management** - Replaced linear search with hash table for O(1) lookup of global variables
- **List Memory Management** - Initialize new slots to NULL after resizing to prevent undefined behavior
- **Module Function Return Values** - Improved handling during VM execution
- **Error Handling** - Enhanced exception handling logic with proper flag management for error propagation
- **Error Logging** - Added detailed error messages for tokenization, parsing, and compilation failures
- **Stack Validation** - Added checks to prevent stack underflow and pointer corruption in VM function calls
- **File Path Handling** - Implemented canonicalization using `realpath` for consistent relative import paths
- **REPL Functionality** - Enhanced to support history and completion only in TTY mode
- **Makefile** - Updated to include linenoise source and gperf-generated keywords hash
- **GitHub Workflows** - Added `gperf` to dependency installations for memory checks, release builds, and tests
- **Script Organization** - Moved testing and utility scripts to `scripts/` directory
- **README Documentation** - Updated to reflect new script paths and third-party libraries section

### Fixed

- **Memory Management** - Fixed memory leaks in list append and iteration operations
- **Stack Management** - Corrected value retention and cleanup order in list operations
- **File Size Check** - Fixed type casting in `builtin_read_file` for proper comparison
- **Error Handling** - Improved error handling in `kronos_run_file` to check for non-zero results
- **Tokenizer** - Fixed error pointer validation before reporting memory allocation failures
- **VM Function Definition** - Corrected function body offset calculations to prevent wrap-around issues
- **Gperf-generated Code** - Fixed function signatures and removed problematic preprocessor directives for cross-platform compatibility
- **Value Equality** - Updated recursive equality checks to use double pointer for visited arrays
- **Release Workflow** - Updated test execution command to use correct script path

### Removed

- **Obsolete Testing Scripts** - Removed outdated LSP and Valgrind testing scripts that were replaced by new implementations

## [0.3.0] - 2025-XX-XX

### Added

- **Logical Operators** - `and`, `or`, `not` operators for boolean logic
- **Lists/Arrays** - Full list operations including literals, indexing, slicing, iteration, and list methods
- **String Operations** - Complete string manipulation suite with concatenation, indexing, slicing, and built-in functions
- **F-Strings** - Formatted string literals with expression interpolation (`f"Hello, {name}!"`)
- **Enhanced Control Flow** - `else if` statements, `break`, and `continue` statements
- **Enhanced Standard Library** - Math functions (`sqrt`, `power`, `abs`, `round`, `floor`, `ceil`, `rand`, `min`, `max`), list utilities (`reverse`, `sort`), and string functions
- **Built-in Math Module** - `import math` with namespaced functions like `math.sqrt`
- **Perfect Hash Function** - Optimized keyword lookup using gperf-generated perfect hash for O(1) keyword matching
- **UTF-8 Identifier Support** - Enhanced tokenizer to support UTF-8 characters in identifiers
- **Multi-line String Support** - Triple-quoted strings (`"""` and `'''`) across multiple lines
- **Escape Sequence Processing** - Proper handling of escape sequences in string literals
- **Configurable Tab Width** - Support for different tab widths in tokenizer for indentation calculations
- **Comment Handling** - Improved tokenizer to handle comments and empty lines correctly
- **AST Position Tracking** - Line and column information for better error reporting
- **AST Documentation** - Pretty-printing functionality and human-readable names for AST nodes
- **Parser Documentation** - Comprehensive function documentation for all parser methods
- **Parser Instance Management** - Creation and cleanup functions for parser lifecycle management

### Changed

- **Keyword Matching** - Replaced linear search with perfect hash function for improved performance
- **Error Reporting** - Enhanced tokenizer error reporting with detailed messages and context
- **Token Memory Management** - Improved token array initialization with configurable capacity
- **String Token Optimization** - Introduced static string constants for single-character tokens
- **Parser Error Handling** - Standardized error handling across parser functions
- **Token Consumption** - Updated to explicitly ignore return values for clarity

### Fixed

- **Mixed Indentation** - Improved error reporting to allow tokenization to continue when spaces and tabs are mixed
- **Unknown Character Handling** - Enhanced tokenizer to report errors for unknown characters
- **AST Node Cleanup** - Added case for `AST_NUMBER` in cleanup function for proper memory handling
- **Token Memory Management** - Fixed token array free functions to ensure consistent behavior

## [0.2.0] - 2025-XX-XX

### Added

- **String Data Type** - Full string support with literals and basic operations
- **String Concatenation** - String concatenation using `plus` operator
- **String Indexing** - Access individual characters using `at` operator
- **String Slicing** - Extract substrings using `from ... to` syntax
- **List Data Type** - List literals with comma-separated values
- **List Indexing** - Access list elements using `at` operator with positive and negative indices
- **List Slicing** - Extract list slices using `from ... to` syntax
- **List Iteration** - Iterate over lists using `for ... in` loops
- **List Built-in Functions** - `len()`, `append()`, and other list utilities
- **Enhanced REPL** - Improved interactive shell with better error messages
- **File Execution** - Execute `.kr` files directly from command line
- **Basic Error Handling** - Improved error messages for syntax and runtime errors
- **Indentation-based Syntax** - Python-like indentation for code blocks
- **Keyword Hash Table** - Efficient keyword lookup using hash table for O(1) average case performance

### Changed

- **Parser Architecture** - Enhanced parser to support more complex expressions and statements
- **VM Instruction Set** - Extended bytecode instructions for list and string operations
- **Memory Management** - Improved garbage collection for list and string types

### Fixed

- **String Memory Leaks** - Fixed memory management issues with string operations
- **List Index Bounds** - Added bounds checking for list indexing operations

## [0.1.0] - 2025-XX-XX

### Added

- **Initial Release** - First public release of the Kronos programming language
- **Core Language Features** - Variables (`set` for immutable, `let` for mutable), basic data types (numbers, booleans, null)
- **Type Annotations** - Optional type annotations using `as` keyword
- **Arithmetic Operators** - Addition (`plus`), subtraction (`minus`), multiplication (`times`), division (`divided by`)
- **Comparison Operators** - Equality (`is equal`), inequality (`is not equal`), greater than (`is greater than`), less than (`is less than`), greater than or equal (`is greater than or equal`), less than or equal (`is less than or equal`)
- **Control Flow** - `if`/`else` statements, `for` loops, `while` loops
- **Functions** - Function definitions with `function ... with ...:`, function calls with `call ... with ...`, parameters, and return values
- **Local Variable Scoping** - Proper scoping rules for local and global variables
- **Built-in Functions** - `print()` for output, `len()` for length operations
- **Built-in Constants** - `pi` constant for mathematical operations
- **REPL** - Interactive read-eval-print loop for testing code snippets
- **Bytecode VM** - Stack-based virtual machine for executing compiled bytecode
- **Reference Counting GC** - Automatic memory management with reference counting
- **Tokenizer** - Lexical analysis converting source code to tokens
- **Parser** - Recursive descent parser building Abstract Syntax Tree
- **Compiler** - AST to bytecode compiler with constant pool management
- **Basic Error Handling** - Syntax error reporting with line and column information
- **Makefile** - Build system for compiling the language implementation
- **Example Files** - Initial set of example `.kr` files demonstrating language features
- **Documentation** - Basic README and project documentation

### Changed

- Initial implementation focused on core language features and VM architecture

### Fixed

- Initial release with basic stability and error handling

---

[Unreleased]: https://github.com/nedanwr/kronos/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/nedanwr/kronos/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/nedanwr/kronos/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/nedanwr/kronos/releases/tag/v0.1.0
