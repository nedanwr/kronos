# Kronos Quick Reference

Fast reference guide for Kronos syntax.

## Variables
```kronos
set name to value
```

## Data Types
- **Numbers:** `42`, `3.14`, `-10`
- **Strings:** `"Hello, World!"`
- **Booleans:** `true`, `false` (future)
- **Nil:** `nil` (future)

## Print
```kronos
print expression
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

