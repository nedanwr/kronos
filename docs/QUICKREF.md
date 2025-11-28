# Kronos Quick Reference

Fast reference guide for Kronos syntax.

## Imports
```kronos
import math
# String functions are global (no import needed)
```

## Variables
```kronos
set name to value
```

## Data Types
- **Numbers:** `42`, `3.14`, `-10`
- **Strings:** `"Hello, World!"`, `f"Hello, {name}!"` (f-strings)
- **Booleans:** `true`, `false`
- **Null:** `null`
- **Lists:** `list 1, 2, 3`

## Print
```kronos
print expression
```

## Strings
```kronos
# Concatenation
set msg to "Hello" plus " " plus "World"

# Indexing
set char to text at 0
set last to text at -1

# Slicing
set slice to text from 0 to 5
set rest to text from 3 to end

# F-strings
set name to "Alice"
set greeting to f"Hello, {name}!"
```

## String Functions
```kronos
call len with "hello"              # Length
call uppercase with "hello"        # "HELLO"
call lowercase with "HELLO"        # "hello"
call trim with "  hello  "         # "hello"
call split with "a,b,c", ","       # list ["a", "b", "c"]
call join with list "a", "b", "c", "-"  # "a-b-c"
call contains with "hello", "ell"  # true
call starts_with with "hello", "he"  # true
call ends_with with "hello", "lo"  # true
call replace with "hello", "l", "L"  # "heLLo"
call to_string with 42             # "42"
```

## Module Functions
```kronos
import math
call math.sqrt with 16             # 4
call math.power with 2, 8          # 256

# String functions are available globally (no import needed)
call uppercase with "hello"        # "HELLO"
call trim with "  text  "          # "text"
```

## Math Functions
```kronos
call sqrt with 16                  # 4
call power with 2, 8               # 256
call abs with -10                  # 10
call round with 3.7                # 4
call floor with 3.9                # 3
call ceil with 3.1                 # 4
call rand with                     # Random 0.0-1.0
call min with 5, 10, 3             # 3
call max with 5, 10, 3             # 10
```

## Type Conversion
```kronos
call to_number with "123"          # 123
call to_bool with "true"           # true
call to_bool with 42               # true
call to_bool with 0                # false
call to_bool with null             # false
```

## List Utilities
```kronos
set original to list 1, 2, 3
call reverse with original         # [3, 2, 1]
call sort with list 3, 1, 2        # [1, 2, 3]
```

## Arithmetic
| Operation | Syntax | Example |
|-----------|--------|---------|
| Addition | `a plus b` | `set sum to 5 plus 3` |
| Subtraction | `a minus b` | `set diff to 10 minus 4` |
| Multiplication | `a times b` | `set prod to 6 times 7` |
| Division | `a divided by b` | `set quot to 20 divided by 4` |

## Comparisons
| Operation | Syntax |
|-----------|--------|
| Equal | `a is equal b` |
| Not equal | `a is not equal b` |
| Greater than | `a is greater than b` |
| Less than | `a is less than b` |

## Conditionals
```kronos
if condition:
    statement
    statement
```

## Loops

### For Loop
```kronos
for variable in range start to end:
    statement
```

### While Loop
```kronos
while condition:
    statement
```

## Code Blocks
- Use `:` to start a block
- Indent with spaces (4 spaces recommended)
- Dedent to end a block

## Examples

### Variables & Print
```kronos
set x to 42
print x
print "Hello"
```

### Arithmetic
```kronos
set result to 5 plus 3 times 2
print result
```

### Conditional
```kronos
if x is greater than 10:
    print "Large"
```

### For Loop
```kronos
for i in range 1 to 5:
    print i
```

### While Loop
```kronos
set count to 5
while count is greater than 0:
    print count
    set count to count minus 1
```

### Nested
```kronos
for i in range 1 to 3:
    if i is equal 2:
        print "Two!"
```

## Running Programs

### Execute File
```bash
./kronos program.kr
```

### REPL
```bash
./kronos
```

### Exit REPL
```
exit
```

## Common Patterns

### Counter Loop
```kronos
set sum to 0
for i in range 1 to 10:
    set sum to sum plus i
print sum
```

### Conditional Counter
```kronos
set count to 0
for i in range 1 to 20:
    if i is greater than 10:
        set count to count plus 1
print count
```

### Nested Loop
```kronos
for i in range 1 to 5:
    for j in range 1 to 5:
        set product to i times j
        print product
```

### While with Condition
```kronos
set x to 0
while x is less than 100:
    print x
    set x to x plus 10
```

## Keywords
- `set` - Variable assignment
- `to` - Assignment operator
- `print` - Output statement
- `plus` - Addition
- `minus` - Subtraction
- `times` - Multiplication
- `divided` - Division
- `by` - Division operator
- `is` - Comparison start
- `equal` - Equality
- `not` - Negation
- `greater` - Greater comparison
- `less` - Less comparison
- `than` - Comparison suffix
- `if` - Conditional
- `for` - For loop
- `in` - Loop operator
- `range` - Range function
- `while` - While loop

## File Extension
All Kronos programs use the `.kr` extension:
- `program.kr`
- `test.kr`
- `main.kr`

## Tips
1. Always initialize variables before use
2. Be careful with infinite loops
3. Use 4-space indentation
4. Break complex expressions into steps
5. Use descriptive variable names

## Error Prevention
```kronos
# Good - initialize before loop
set sum to 0
for i in range 1 to 10:
    set sum to sum plus i

# Bad - undefined variable
for i in range 1 to 10:
    set sum to sum plus i  # Error: sum undefined

# Good - avoid infinite loop
set count to 0
while count is less than 10:
    print count
    set count to count plus 1  # Don't forget!

# Bad - infinite loop
set count to 0
while count is less than 10:
    print count
    # Missing increment!
```

---

For detailed explanations, see [SYNTAX.md](SYNTAX.md)

