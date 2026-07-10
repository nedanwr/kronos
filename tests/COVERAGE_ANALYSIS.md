# Test Coverage Analysis

## Snapshot (2026-03-14)

- Unit tests: **188** (run via `make test-unit` or `./scripts/run_tests.sh`)
- Integration tests: **165** total
  - Passing tests: **104** (`tests/integration/pass`)
  - Expected-fail tests: **61** (`tests/integration/fail`)
- LSP tests: **52** (`tests/lsp/test_lsp_features.c`, run via `make test-lsp`)
- Memory checks: `./scripts/valgrind_docker.sh` passes across all integration tests

## Current Coverage Status

### Well covered

- **Core language syntax and semantics**
  - Variables: immutable, mutable, typed, reassignment rules
  - Types: primitive, generic, union, alias/map-shape checks
  - Operators: arithmetic, modulo, comparison, logical, unary negation
  - Control flow: if/else-if/else, for/while, break/continue, match/case/default
  - Functions: params, defaults, variadic params, named args, lambdas, local scope
  - Multi-return/tuple unpacking and swap assignments
  - Imports/modules: built-in modules and file-based modules
  - Exceptions: raise, catch-all, typed catch, runtime-error catch, finally

- **Data structure features**
  - Strings: indexing, slicing, methods/functions, f-strings (including format specs)
  - Lists: literals, indexing/slicing, assignment, iteration, comprehensions
  - Maps: literals, set/get, deletion, key type behavior
  - Ranges: construction, step, indexing, slicing, length, iteration

- **Standard library and built-ins**
  - Math built-ins (including `rand`)
  - String functions
  - Collection helpers (`len`, `reverse`, `sort`, `filter`, `map`)
  - Type conversion helpers (`to_string`, `to_number`, `to_bool`)
  - Regex helpers (`match`, `search`, `findall`)
  - File/path I/O helpers (`read_file`, `write_file`, `read_lines`, `list_files`, etc.)
  - All built-ins registered in `src/vm/vm_builtins_registry.c` are exercised by tests

- **Unit-level engine coverage**
  - Tokenizer: keywords/operators, multiline strings, escape sequences, UTF-8 identifiers, indentation recovery
  - Parser: major AST forms including match, type alias, list comprehension, break/continue, imports, f-strings
  - Runtime: value creation/equality/truthiness/type-checking, string interning, list/map/range operations
  - Compiler: core statement/expression lowering paths
  - VM: execution behavior for arithmetic/comparison/logical ops, loops, functions, comprehensions, type checks, stack-underflow safety tests
  - GC: allocation tracking, object count/bytes accounting, cycle collection, cleanup behavior

### Error-path coverage

- 61 expected-fail integration tests verify runtime and semantic error handling:
  - Undefined symbols, type mismatches, invalid argument counts/types
  - Division/modulo by zero
  - Out-of-bounds and wrong-index-type errors
  - Module loading/circular-import failures
  - Regex/type-conversion/file-I/O failure paths
  - Pattern matching and type-alias validation errors

## Remaining Gaps and Risks

- **Partially specified behavior:** `try/finally` now has explicit integration coverage for the non-throwing path (`tests/integration/pass/exception_finally_no_catch.kr`), but there is still no passing test that asserts finally-body execution when an exception is thrown without a catch.

- **Coverage is reported but not enforced**
  - `make coverage` produces line/branch summaries using lcov.
  - CI now has an optional coverage-report job, but no minimum thresholds are enforced.

- **Local default runner excludes LSP**
  - `./scripts/run_tests.sh` still focuses on unit + integration tests.
  - CI now gates on LSP via `.github/workflows/test.yml` (`make test-lsp` job).

## Recommended Next Steps

1. Add a dedicated regression test for `try/finally` when the try block throws and no catch is present, and confirm intended semantics.
2. Optionally extend `make coverage` runs with `COVERAGE_INCLUDE_LSP=1` to fold LSP execution paths into the same report.
3. If desired, add coverage thresholds (line/branch minimums) to CI once baseline percentages stabilize.
