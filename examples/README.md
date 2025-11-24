# Kronos Examples

This directory contains example programs demonstrating various features of the Kronos language.

## Basic Examples

### hello.kr

Simple hello world program demonstrating:

- String printing
- Variable assignment
- Basic output

**Run:**

```bash
./kronos examples/hello.kr
```

### variables.kr

Demonstration of variable features:

- Mutable variables (`let`)
- Immutable variables (`set`)
- Type annotations (`as <type>`)
- Boolean literals (`true`, `false`)
- Null values

**Run:**

```bash
./kronos examples/variables.kr
```

## Feature Examples

### arithmetic.kr

Comprehensive arithmetic operations:

- Addition (`plus`)
- Subtraction (`minus`)
- Multiplication (`times`)
- Division (`divided by`)

**Run:**

```bash
./kronos examples/arithmetic.kr
```

### conditionals.kr

Conditional statement examples:

- `if` statements
- Comparison operators
  - `is greater than`
  - `is less than`
  - `is equal`

**Run:**

```bash
./kronos examples/conditionals.kr
```

### loops.kr

Loop demonstrations:

- `for` loops with `range`
- `while` loops
- Loop counters and conditions

**Run:**

```bash
./kronos examples/loops.kr
```

### control_flow.kr

Showcases the newest control flow features:

- `else if` / `else` chains
- `break` and `continue`
- Range loops with `by <step>`
- Basic module imports (`import math`)

**Run:**

```bash
./kronos examples/control_flow.kr
```

## Advanced Examples

### syntax_showcase.kr

Comprehensive demonstration of all language features:

- Variables
- Arithmetic
- Comparisons
- For loops
- While loops
- Nested structures
- Complex expressions
- Conditional logic

**Run:**

```bash
./kronos examples/syntax_showcase.kr
```

### fizzbuzz.kr

FizzBuzz implementation showing:

- Loop iteration
- Modulo-like operations
- Multiple conditionals
- Complex logic

**Run:**

```bash
./kronos examples/fizzbuzz.kr
```

### functions_simple.kr

Simple function examples:

- Function definitions with parameters
- Function calls
- Return values
- Local variables

**Run:**

```bash
./kronos examples/functions_simple.kr
```

### functions.kr

Comprehensive function demonstrations:

- Functions with one parameter
- Functions with multiple statements
- Functions with arithmetic
- String parameters
- Return value handling
- Local variable scoping
- Built-in Pi constant usage

**Run:**

```bash
./kronos examples/functions.kr
```

### builtins.kr

Built-in constants and functions:

- Pi mathematical constant
- Built-in math functions (add, subtract, multiply, divide)
- Using built-ins within user functions
- Demonstration of reserved function names

**Run:**

```bash
./kronos examples/builtins.kr
```

## Running Examples

### Run a specific example:

```bash
./kronos examples/<filename>.kr
```

### Run all examples:

```bash
for file in examples/*.kr; do
    echo "=== Running $file ==="
    ./kronos "$file"
    echo ""
done
```

## Creating Your Own Examples

1. Create a new file with `.kr` extension
2. Write your Kronos code
3. Run with: `./kronos your_file.kr`

### Template:

```kronos
print "My Program"
print ""

set x to 10
print x

for i in range 1 to 5:
    print i
```

## Example Output

Each example produces formatted output showing the results of the operations. For instance:

**hello.kr output:**

```
Hello, World!
Welcome to Kronos programming language!
Nice to meet you!
```

**arithmetic.kr output:**

```
Testing arithmetic operations:
13
7
30
3.33333
```

## Learning Path

Recommended order for learning:

1. **hello.kr** - Start here for basics
2. **variables.kr** - Learn variables (mutable/immutable, types)
3. **arithmetic.kr** - Learn arithmetic operations
4. **conditionals.kr** - Understand if statements
5. **loops.kr** - Master loops
6. **functions_simple.kr** - Learn functions
7. **builtins.kr** - Learn built-in constants and functions
8. **syntax_showcase.kr** - See everything together
9. **fizzbuzz.kr** - Apply your knowledge
10. **functions.kr** - Advanced function examples

## Troubleshooting

### "File not found" error

Make sure you're in the kronos root directory:

```bash
cd /path/to/kronos
./kronos examples/hello.kr
```

### Syntax errors

Check that:

- Colons (`:`) are placed after conditions/loop headers
- Indentation is consistent (4 spaces recommended)
- All variables are defined before use

### Unexpected output

Verify:

- Variable names are spelled correctly
- Arithmetic operations use correct keywords
- Loop ranges are set properly

## Contributing Examples

Have a great example? Consider adding it:

1. Create a `.kr` file in this directory
2. Add documentation comments (when supported)
3. Test thoroughly
4. Update this README

## Additional Resources

- [SYNTAX.md](../docs/SYNTAX.md) - Complete syntax reference
- [QUICKREF.md](../docs/QUICKREF.md) - Quick reference guide
- [README.md](../README.md) - Main project documentation

---

Happy coding in Kronos! 🚀
