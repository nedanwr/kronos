# Kronos Test Suite

Comprehensive test suite for the Kronos programming language.

## Directory Structure

```
tests/
├── pass/        # Tests that should execute successfully
└── fail/        # Tests that should produce errors
```

## Running Tests

### Run All Tests

```bash
./run_tests.sh
```

### Run Specific Category

```bash
# Run only passing tests
./kronos tests/pass/*.kr

# Run only error tests (each will produce an error)
./kronos tests/fail/01_immutable_reassign.kr
```

## Test Categories

### Passing Tests (`tests/pass/`)

| Test File                 | Description                        |
| ------------------------- | ---------------------------------- |
| `variables_immutable.kr`  | Immutable variables with `set`     |
| `variables_mutable.kr`    | Mutable variables with `let`       |
| `variables_typed.kr`      | Type-annotated variables           |
| `arithmetic_basic.kr`     | Basic arithmetic operations        |
| `arithmetic_complex.kr`   | Complex arithmetic expressions     |
| `comparisons.kr`          | Comparison operations              |
| `conditionals.kr`         | If statements                      |
| `functions_simple.kr`     | Simple function definitions        |
| `functions_params.kr`     | Functions with multiple parameters |
| `functions_local_vars.kr` | Functions with local variables     |
| `builtins_pi.kr`          | Pi constant usage                  |
| `builtins_math.kr`        | Built-in math functions            |
| `booleans.kr`             | Boolean literals                   |
| `null_values.kr`          | Null values                        |

### Error Tests (`tests/fail/`)

| Test File                   | Expected Error                     |
| --------------------------- | ---------------------------------- |
| `immutable_reassign.kr`     | Cannot reassign immutable variable |
| `type_mismatch.kr`          | Type mismatch for typed variable   |
| `undefined_variable.kr`     | Undefined variable                 |
| `division_by_zero.kr`       | Cannot divide by zero              |
| `type_error_arithmetic.kr`  | Type error in arithmetic           |
| `function_too_many_args.kr` | Function called with too many args |
| `function_too_few_args.kr`  | Function called with too few args  |
| `undefined_function.kr`     | Undefined function                 |
| `pi_reassign.kr`            | Cannot reassign Pi constant        |
| `builtin_wrong_args.kr`     | Built-in with wrong arg count      |
| `builtin_wrong_types.kr`    | Built-in with wrong arg types      |
| `comparison_type_error.kr`  | Comparison type error              |

Together these cover 26 comprehensive tests (14 passing + 12 error cases).

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

1. Create a new file in `tests/pass/` with a descriptive name
2. Add a comment describing what the test validates
3. Write Kronos code that should execute successfully
4. Run `./run_tests.sh` to verify

Example:

```kronos
# Test: String concatenation (future feature)
# Expected: Pass

set greeting to "Hello, " plus "World!"
print greeting
```

### For Error Tests

1. Create a new file in `tests/fail/` with a descriptive name
2. Add a comment describing the expected error
3. Write Kronos code that should produce an error
4. Run `./run_tests.sh` to verify

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

Total tests:  26
Passed:       14
Failed:       12

Success rate: 54%

✓ TESTS COMPLETED
```

## Debugging Failed Tests

If a test fails:

1. Run the specific test file directly:

   ```bash
   ./kronos tests/pass/01_variables_immutable.kr
   ```

2. Check the output and error messages

3. Compare with the expected behavior in the test file's comment

4. Use `make clean && make` if you suspect a build issue

## Performance Tests

For performance testing, use the dedicated `benchmarks/` directory (future).

## Related Documentation

- [SAFETY_CHECKS.md](../docs/SAFETY_CHECKS.md) - All safety checks and error messages
- [SYNTAX.md](../docs/SYNTAX.md) - Language syntax reference
- [PROJECT.md](../docs/PROJECT.md) - Feature implementation status and roadmap
