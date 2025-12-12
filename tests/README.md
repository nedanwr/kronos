# Kronos Test Suite

Comprehensive test suite for the Kronos programming language.

## Directory Structure

```
tests/
├── integration/
│   ├── pass/    # Tests that should execute successfully
│   └── fail/    # Tests that should produce errors
├── unit/        # Unit tests for individual components
└── framework/   # Test framework code
```

## Running Tests

### Run All Tests

```bash
./scripts/run_tests.sh
```

### Run Specific Category

```bash
# Run only passing tests
./kronos tests/integration/pass/*.kr

# Run only error tests (each will produce an error)
./kronos tests/integration/fail/immutable_reassign.kr
```

## Test Categories

### Passing Tests (`tests/integration/pass/`)

The test suite includes 42 passing integration tests covering:

- Variables (immutable, mutable, typed)
- Arithmetic operations
- Comparisons and logical operators
- Control flow (if, for, while, break, continue)
- Functions (simple, parameters, local variables)
- Built-in constants and functions
- Lists (creation, indexing, slicing, iteration)
- Strings (operations, functions, methods)
- F-strings (string interpolation)
- Type conversion
- Range objects
- Module imports

### Error Tests (`tests/integration/fail/`)

The test suite includes 29 error tests covering:

- Immutability violations
- Type mismatches
- Undefined variables and functions
- Division by zero
- Index out of bounds
- Wrong argument counts/types
- Invalid operations

Together these cover 71 comprehensive tests (42 passing + 29 error cases).

## Test Coverage

The test suite covers:

✅ **Variables**

- Immutable (`set`) and mutable (`let`) variables
- Type annotations (`as <type>`)
- Type enforcement
- Undefined variable detection

✅ **Functions**

- Function definitions and calls
- Multiple parameters
- Local variables
- Return values
- Argument count validation

✅ **Arithmetic**

- Basic operations: `plus`, `minus`, `times`, `divided by`
- Complex expressions
- Type checking
- Division by zero prevention

✅ **Comparisons**

- Equality: `is equal`, `is not equal`
- Relational: `is greater than`, `is less than`
- Type checking

✅ **Control Flow**

- If statements with conditions
- For loops with ranges
- While loops

✅ **Built-ins**

- Pi constant (immutable)
- Math functions: `add`, `subtract`, `multiply`, `divide`
- Argument validation
- Type checking

✅ **Data Types**

- Numbers (integers and floats)
- Strings
- Booleans (`true`, `false`)
- Null values

✅ **Error Handling**

- Human-readable error messages
- Immutability enforcement
- Type safety
- Undefined reference detection

## CI/CD Integration

Tests are automatically run on every commit via GitHub Actions.

See `.github/workflows/test.yml` for the CI configuration.

## Adding New Tests

### For Passing Tests

1. Create a new file in `tests/integration/pass/` with a descriptive name
2. Add a comment describing what the test validates
3. Write Kronos code that should execute successfully
4. Run `./scripts/run_tests.sh` to verify

Example:

```kronos
# Test: String concatenation (future feature)
# Expected: Pass

set greeting to "Hello, " plus "World!"
print greeting
```

### For Error Tests

1. Create a new file in `tests/integration/fail/` with a descriptive name
2. Add a comment describing the expected error
3. Write Kronos code that should produce an error
4. Run `./scripts/run_tests.sh` to verify

Example:

```kronos
# Test: Using reserved keyword as variable
# Expected: Error: 'function' is a reserved keyword

set function to 10
```

## Test Output Format

### Successful Run

```
════════════════════════════════════════════════════════════
              KRONOS TEST SUITE
════════════════════════════════════════════════════════════

Building Kronos...
✓ Build successful

Running passing tests...
────────────────────────────────────────────────────────────
✓ PASS: 01_variables_immutable
✓ PASS: 02_variables_mutable
...

Running error tests...
────────────────────────────────────────────────────────────
✓ PASS: 01_immutable_reassign
   ↳ Error: Cannot reassign immutable variable 'x'
✓ PASS: 02_type_mismatch
   ↳ Error: Type mismatch for variable 'age': expected 'number'
...

════════════════════════════════════════════════════════════
                    TEST RESULTS
════════════════════════════════════════════════════════════

Total tests:  71
Passed:       42
Failed:       29

Success rate: 59%

✓ TESTS COMPLETED
```

## Debugging Failed Tests

If a test fails:

1. Run the specific test file directly:

   ```bash
   ./kronos tests/integration/pass/variables_immutable.kr
   ```

2. Check the output and error messages

3. Compare with the expected behavior in the test file's comment

4. Use `make clean && make` if you suspect a build issue

## Performance Tests

For performance testing, use the dedicated `benchmarks/` directory (future).

## Related Documentation

- [SYNTAX.md](../docs/SYNTAX.md) - Language syntax reference
- [PROJECT.md](../docs/PROJECT.md) - Architecture and implementation details
- [COVERAGE_ANALYSIS.md](COVERAGE_ANALYSIS.md) - Test coverage analysis
