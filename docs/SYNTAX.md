# Kronos Language Syntax Reference

Complete syntax guide for the Kronos programming language - a human-readable language with natural language keywords.

## Table of Contents

1. [Variables](#variables)
2. [Data Types](#data-types)
3. [Printing](#printing)
4. [Arithmetic Operations](#arithmetic-operations)
5. [Comparison Operators](#comparison-operators)
6. [Logical Operators](#logical-operators)
7. [Conditional Statements](#conditional-statements)
8. [Loops](#loops)
9. [Built-in Constants and Functions](#built-in-constants-and-functions)
10. [Functions](#functions)
11. [String Operations](#string-operations)
12. [Lists and Maps](#lists-and-maps)
13. [Safety & Error Handling](#safety--error-handling)
14. [Comments](#comments)
15. [Indentation](#indentation)

---

## Variables

Kronos supports two types of variables: **immutable** (using `set`) and **mutable** (using `let`).

### Immutable Variables (set)

Immutable variables cannot be reassigned after their initial value is set.

**Syntax:**
```
set <variable_name> to <value>
```

**Examples:**
```kronos
set x to 5
set name to "Alice"
set pi to 3.14159
```

Attempting to reassign will result in an error:
```kronos
set x to 10
set x to 20  # Error: Cannot reassign immutable variable 'x'
```

### Mutable Variables (let)

Mutable variables can be reassigned to new values.

**Syntax:**
```
let <variable_name> to <value>
```

**Examples:**
```kronos
let counter to 0
let counter to 1
let counter to 2  # OK: counter is mutable
```

### Type Annotations (Optional)

You can optionally specify a type using the `as` keyword. Once specified, the variable can only hold values of that type.

**Syntax:**
```
set <variable_name> to <value> as <type>
let <variable_name> to <value> as <type>
```

**Available Types:**
- `number` - Integers and floating-point numbers
- `string` - Text values
- `boolean` - `true` or `false`
- `null` - Represents no value
- `list` - Ordered collections of values
- `map` - Key-value collections

**Examples:**
```kronos
let age to 25 as number
let age to 26 as number       # OK: still a number
let age to "twenty"           # Error: Type mismatch

set name to "Bob" as string
let flag to true as boolean
let flag to false as boolean  # OK: still a boolean
```

### Rules

- Variable names must start with a letter or underscore
- Can contain letters, numbers, and underscores
- Case sensitive (`myVar` and `myvar` are different)
- No keywords can be used as variable names
- Immutable variables (`set`) cannot be reassigned
- Type-annotated variables cannot change types

---

## Data Types

Kronos supports several built-in data types:

### Numbers

Both integers and floating-point numbers are supported.

```kronos
set age to 25
set temperature to 98.6
set negative to -10
set decimal to 0.5
```

### Strings

Text enclosed in double quotes.

```kronos
set greeting to "Hello, World!"
set empty to ""
set multiword to "This is a sentence"
```

**F-Strings (Formatted String Literals):**

F-strings allow embedding expressions inside strings using `f"..."` or `f'...'` syntax.

```kronos
set name to "Alice"
set greeting to f"Hello, {name}!"
print greeting  # Output: Hello, Alice!

set x to 5
set y to 10
set result to f"{x} plus {y} equals {x plus y}"
print result  # Output: 5 plus 10 equals 15
```

**Features:**
- Expression evaluation inside `{ }` blocks
- Automatic type conversion (numbers, booleans, null → strings)
- Multiple expressions in a single f-string
- Nested expressions and function calls

### Booleans

True or false values using the `true` and `false` keywords.

```kronos
set isActive to true
set isComplete to false
let flag to true
let flag to false
```

### Null

Represents no value using the `null` keyword.

```kronos
set nothing to null
let result to null
```

---

## Printing

The `print` statement outputs values to the console.

### Syntax

```
print <expression>
```

### Examples

**Print literal values:**

```kronos
print "Hello, World!"
print 42
print 3.14
```

**Print variables:**

```kronos
set name to "Bob"
print name
```

**Print expressions:**

```kronos
set x to 10
set y to 20
print x plus y
```

---

## Arithmetic Operations

Kronos uses human-readable words for arithmetic operations.

### Addition

```
<value> plus <value>
```

```kronos
set sum to 5 plus 3
print sum
```

### Subtraction

```
<value> minus <value>
```

```kronos
set difference to 10 minus 4
print difference
```

### Multiplication

```
<value> times <value>
```

```kronos
set product to 6 times 7
print product
```

### Division

```
<value> divided by <value>
```

```kronos
set quotient to 20 divided by 4
print quotient
```

### Modulo

```
<value> mod <value>
```

Returns the remainder after division.

```kronos
set remainder to 10 mod 3
print remainder  # 1

set result to 100 mod 7
print result  # 2
```

**Note:** Modulo by zero will result in an error.

### Unary Negation

```
-<value>
```

Negates a number (makes it negative if positive, or positive if negative).

```kronos
set neg to -5
print neg  # -5

set x to 10
set neg_x to -x
print neg_x  # -10

set result to -5 plus 3
print result  # -2
```

**Note:** Double negation (`--x`) is supported for multiple levels of negation.

### Complex Expressions

Operations are evaluated left to right. Use variables to control order:

```kronos
set a to 2 plus 3 times 4
print a

set step1 to 3 times 4
set b to 2 plus step1
print b
```

### Examples

```kronos
set x to 10
set y to 3

set sum to x plus y
set diff to x minus y
set prod to x times y
set quot to x divided by y
set rem to x mod y

print sum
print diff
print prod
print quot
print rem
```

---

## Comparison Operators

Comparisons return boolean values and are used in conditional statements.

### Equal

```
<value> is equal <value>
```

```kronos
set a to 5
set b to 5
if a is equal b:
    print "a equals b"
```

### Not Equal

```
<value> is not equal <value>
```

```kronos
set x to 10
set y to 20
if x is not equal y:
    print "x is not equal to y"
```

### Greater Than

```
<value> is greater than <value>
```

```kronos
set score to 85
if score is greater than 70:
    print "Passing grade"
```

### Less Than

```
<value> is less than <value>
```

```kronos
set temperature to 32
if temperature is less than 50:
    print "It's cold"
```

### Greater Than or Equal (future)

```
<value> is greater than or equal <value>
```

### Less Than or Equal (future)

```
<value> is less than or equal <value>
```

---

## Logical Operators

Kronos supports logical operators for combining boolean expressions: `and`, `or`, and `not`.

### AND Operator

The `and` operator returns `true` only if both operands are `true`.

**Syntax:**
```
<expression> and <expression>
```

**Examples:**
```kronos
set x to 7
if x is greater than 5 and x is less than 10:
    print "x is between 5 and 10"

set age to 25
set hasLicense to true
if age is greater than 18 and hasLicense is equal true:
    print "Can drive"
```

### OR Operator

The `or` operator returns `true` if at least one operand is `true`.

**Syntax:**
```
<expression> or <expression>
```

**Examples:**
```kronos
set x to 0
set y to 5
if x is equal 0 or y is equal 0:
    print "At least one is zero"

set isAdmin to true
set isModerator to false
if isAdmin is equal true or isModerator is equal true:
    print "access granted"
```

### NOT Operator

The `not` operator negates a boolean expression.

**Syntax:**
```
not <expression>
```

**Examples:**
```kronos
set isDisabled to false
if not isDisabled:
    print "enabled"

set x to 10
if not (x is equal 5):
    print "x is not 5"
```

### Operator Precedence

Logical operators have lower precedence than comparison operators, so comparisons are evaluated first:

```kronos
# This is evaluated as: (x > 5) and (x < 10)
if x is greater than 5 and x is less than 10:
    print "between"

# Use parentheses for clarity when needed
if (x is equal 0 or y is equal 0) and z is greater than 0:
    print "condition met"
```

### Notes

- Logical operators work with boolean values
- Non-boolean values are converted to booleans using truthiness rules
- `and` and `or` support short-circuit evaluation
- `not` is a unary operator (takes one operand)

---

## Conditional Statements

Execute code based on conditions.

### If Statement

**Syntax:**

```
if <condition>:
    <indented block>
```

**Examples:**

Simple condition:

```kronos
set age to 18
if age is greater than 17:
    print "You are an adult"
```

With variable assignment:

```kronos
set score to 95
if score is greater than 90:
    set grade to "A"
    print grade
```

Multiple statements:

```kronos
set balance to 1000
if balance is greater than 500:
    print "Sufficient funds"
    set canPurchase to true
    print canPurchase
```

Nested conditions:

```kronos
set x to 10
if x is greater than 5:
    print "x is greater than 5"
    if x is less than 15:
        print "x is also less than 15"
```

### Else If Statement

Chain multiple conditions together.

**Syntax:**

```
if <condition>:
    <indented block>
else if <condition>:
    <indented block>
else if <condition>:
    <indented block>
else:
    <indented block>
```

**Examples:**

Grade assignment:

```kronos
set score to 85

if score is greater than 90:
    print "A"
else if score is greater than 80:
    print "B"
else if score is greater than 70:
    print "C"
else:
    print "F"
```

### Else Statement

Execute code when the condition is false.

**Syntax:**

```
if <condition>:
    <indented block>
else:
    <indented block>
```

**Examples:**

```kronos
set age to 18
if age is greater than 17:
    print "You are an adult"
else:
    print "You are a minor"
```

---

## Loops

Repeat code multiple times.

### For Loop

Iterate over a range of numbers.

**Syntax:**

```
for <variable> in range <start> to <end> [by <step>]:
    <indented block>
```

The `by <step>` clause is optional. If omitted, the step defaults to 1.

**Examples:**

Count from 1 to 5:

```kronos
for i in range 1 to 5:
    print i
```

Count from 0 to 10:

```kronos
for counter in range 0 to 10:
    print counter
```

Use loop variable in calculations:

```kronos
for i in range 1 to 10:
    set square to i times i
    print square
```

Nested loops:

```kronos
for i in range 1 to 3:
    for j in range 1 to 3:
        set product to i times j
        print product
```

Range with step:

```kronos
# Count by 2s
for i in range 0 to 10 by 2:
    print i
# Prints: 0, 2, 4, 6, 8, 10

# Count by 5s
for i in range 0 to 100 by 5:
    print i
# Prints: 0, 5, 10, 15, ..., 100
```

**Range behavior:**

- `range <start> to <end>` is inclusive on both ends
- `range 1 to 5` includes 1, 2, 3, 4, and 5
- The step value determines the increment between iterations
- Step defaults to 1 if not specified

### While Loop

Repeat while a condition is true.

**Syntax:**

```
while <condition>:
    <indented block>
```

**Examples:**

Count up:

```kronos
set count to 1
while count is less than 6:
    print count
    set count to count plus 1
```

Count down:

```kronos
set countdown to 10
while countdown is greater than 0:
    print countdown
    set countdown to countdown minus 1
```

With complex conditions:

```kronos
set x to 0
set limit to 100
while x is less than limit:
    print x
    set x to x plus 10
```

**Warning:** Be careful to update variables in the loop to avoid infinite loops!

### Break Statement

Exit a loop early.

**Syntax:**

```
break
```

**Examples:**

```kronos
for i in range 1 to 10:
    if i is equal 5:
        break
    print i
# Prints: 1, 2, 3, 4
```

### Continue Statement

Skip to the next iteration of a loop.

**Syntax:**

```
continue
```

**Examples:**

```kronos
for i in range 1 to 10:
    if i is equal 5:
        continue
    print i
# Prints: 1, 2, 3, 4, 6, 7, 8, 9, 10
```

**Note:** `break` and `continue` can only be used inside loops (`for` or `while`).

---

## Modules and Imports

Kronos supports importing both built-in modules and file-based modules to organize code into namespaces.

### Importing Built-in Modules

**Syntax:**
```
import <module_name>
```

**Available Built-in Modules:**
- `math` - Mathematical functions (sqrt, power, abs, round, floor, ceil, rand, min, max)

**Note:** String functions (uppercase, lowercase, trim, split, join, contains, starts_with, ends_with, replace, len, to_string) are available globally and do not require an import.

**Examples:**
```kronos
import math
# String functions are global (no import needed)
```

### Importing File-based Modules

You can import code from other `.kr` files to organize your code into reusable modules.

**Syntax:**
```
import <module_name> from "<file_path>"
```

**Examples:**
```kronos
# Import a module from a file
import utils from "utils.kr"
import math_utils from "lib/math_utils.kr"

# Relative paths are supported
import helpers from "./helpers.kr"
```

**Module File Execution:**
When a module is imported, the entire module file is executed in its own VM context. This means:
- All top-level statements in the module file are executed
- Functions and variables defined in the module are available through the module namespace
- Modules are cached - importing the same module multiple times only loads it once

**Example Module File (`utils.kr`):**
```kronos
# Utility functions module
print "Loading utils module..."

function greet name:
    print "Hello, " + name + "!"

set version to "1.0.0"
```

**Using the Module:**
```kronos
import utils from "utils.kr"
# Module code executes here (prints "Loading utils module...")

# Access module functions (when fully implemented)
# call utils.greet with "Alice"
```

### Using Module Functions

After importing a module, access its functions using the `module.function` syntax:

```kronos
import math
set result to call math.sqrt with 16        # Returns 4
set power to call math.power with 2, 8      # Returns 256

# String functions are available globally (no import needed)
set upper to call uppercase with "hello"    # Returns "HELLO"
set trimmed to call trim with "  text  "    # Returns "text"
```

**Note:** Math functions can be accessed via the `math` module namespace. String functions are always available globally without any import, similar to Python and TypeScript.

---

## Built-in Constants and Functions

Kronos provides several built-in constants and functions for common operations.

### Built-in Constants

**Pi** - Mathematical constant π (pi), accurate to 100 decimal places

```kronos
print Pi
# Output: 3.14159...

set radius to 5
set area to Pi times radius times radius
print area
# Output: 78.5398
```

**Protection Rules:**
- `Pi` is immutable and cannot be reassigned
- `Pi` cannot be used as a function parameter name
- Attempting either will result in an error

```kronos
set Pi to 3.14  # Error: Cannot reassign immutable variable 'Pi'
```

The Pi constant is available globally and can be used in any calculation.

### Built-in Functions

**Mathematical Operations:**

- `add(a, b)` - Adds two numbers
- `subtract(a, b)` - Subtracts b from a
- `multiply(a, b)` - Multiplies two numbers
- `divide(a, b)` - Divides a by b
- `sqrt(number)` - Square root of a number (requires non-negative number)
- `power(base, exponent)` - Raises base to the power of exponent
- `abs(number)` - Absolute value of a number
- `round(number)` - Rounds number to nearest integer
- `floor(number)` - Floor (rounds down) of a number
- `ceil(number)` - Ceiling (rounds up) of a number
- `rand()` - Returns a random number between 0.0 and 1.0
- `min(...)` - Returns the minimum of one or more numbers
- `max(...)` - Returns the maximum of one or more numbers

**Examples:**

```kronos
call add with 10, 5        # Returns 15
call subtract with 10, 5   # Returns 5
call multiply with 10, 5   # Returns 50
call divide with 10, 5     # Returns 2
call sqrt with 16          # Returns 4
call power with 2, 8       # Returns 256
call abs with -10          # Returns 10
call round with 3.7        # Returns 4
call floor with 3.9        # Returns 3
call ceil with 3.1         # Returns 4
call rand with             # Returns random number 0.0-1.0
call min with 5, 10, 3     # Returns 3
call max with 5, 10, 3     # Returns 10
```

**String Operations:**

- `len(value)` - Get length of string or list
- `uppercase(string)` - Convert string to uppercase
- `lowercase(string)` - Convert string to lowercase
- `trim(string)` - Remove leading and trailing whitespace
- `split(string, delimiter)` - Split string into list by delimiter
- `join(list, delimiter)` - Join list of strings with delimiter
- `to_string(value)` - Convert any value to string representation
- `contains(string, substring)` - Check if string contains substring (returns boolean)
- `starts_with(string, prefix)` - Check if string starts with prefix (returns boolean)
- `ends_with(string, suffix)` - Check if string ends with suffix (returns boolean)
- `replace(string, old, new)` - Replace all occurrences of old substring with new

**Type Conversion:**

- `to_string(value)` - Convert any value to string representation
- `to_number(string)` - Convert string to number (also accepts numbers)
- `to_bool(value)` - Convert value to boolean (strings "true"/"false", numbers, null)

**List Utilities:**

- `reverse(list)` - Returns a new list with elements in reverse order
- `sort(list)` - Returns a new sorted list (numbers or strings only, all same type)

**String Function Examples:**

```kronos
set text to "Hello World"
set length to call len with text              # Returns 11
set upper to call uppercase with text         # Returns "HELLO WORLD"
set trimmed to call trim with "  hello  "     # Returns "hello"
set parts to call split with "a,b,c", ","     # Returns list ["a", "b", "c"]
set joined to call join with parts, "-"       # Returns "a-b-c"
set has_ello to call contains with text, "ello"  # Returns true
set replaced to call replace with text, "World", "Kronos"  # Returns "Hello Kronos"
```

**Type Conversion Examples:**

```kronos
set num_str to "123"
set num_val to call to_number with num_str    # Returns 123
set bool_val to call to_bool with "true"      # Returns true
set bool_val2 to call to_bool with 42         # Returns true
set bool_val3 to call to_bool with 0          # Returns false
set bool_val4 to call to_bool with null       # Returns false
```

**List Utility Examples:**

```kronos
set original to list 1, 2, 3, 4, 5
set reversed to call reverse with original    # Returns [5, 4, 3, 2, 1]
set unsorted to list 3, 1, 4, 1, 5
set sorted to call sort with unsorted         # Returns [1, 1, 3, 4, 5]
set words to list "zebra", "apple", "banana"
set sorted_words to call sort with words      # Returns ["apple", "banana", "zebra"]
```

**File I/O Operations:**

- `read_file(path)` - Reads entire file content as a string
- `write_file(path, content)` - Writes string content to a file (returns nil on success)
- `read_lines(path)` - Reads file and returns a list of lines (one string per line)
- `file_exists(path)` - Checks if a file or directory exists (returns boolean)
- `list_files(path)` - Lists files in a directory (returns list of file names as strings)

**Examples:**

```kronos
# Read a file
set content to call read_file with "data.txt"
print content

# Write to a file
call write_file with "output.txt", "Hello, World!"

# Read file line by line
set lines to call read_lines with "data.txt"
for line in lines:
    print line

# Check if file exists
set exists to call file_exists with "data.txt"
if exists:
    print "File exists!"

# List files in directory
set files to call list_files with "."
for file in files:
    print file
```

**Path Operations:**

- `join_path(path1, path2)` - Joins two path components with appropriate separator
- `dirname(path)` - Returns the directory name from a file path
- `basename(path)` - Returns the file name from a file path

**Examples:**

```kronos
# Join paths
set full_path to call join_path with "dir", "file.txt"        # Returns "dir/file.txt"
set full_path2 to call join_path with "/path/to", "file.txt"  # Returns "/path/to/file.txt"

# Get directory name
set dir to call dirname with "/path/to/file.txt"  # Returns "/path/to"
set dir2 to call dirname with "file.txt"          # Returns "."

# Get file name
set file to call basename with "/path/to/file.txt"  # Returns "file.txt"
set file2 to call basename with "dir/subdir"        # Returns "subdir"
```

**Regular Expressions:**

The `regex` module provides pattern matching using POSIX extended regular expressions.

- `regex.match(string, pattern)` - Returns `true` if pattern matches entire string, `false` otherwise
- `regex.search(string, pattern)` - Returns first matched substring or `null` if no match
- `regex.findall(string, pattern)` - Returns list of all matched substrings (empty list if no matches)

**Examples:**
```kronos
# Check if pattern matches entire string
set matches to call regex.match with "hello", "h.*o"
print matches  # Prints: true

# Find first match
set result to call regex.search with "hello world", "world"
print result  # Prints: world

# Find all matches
set all_matches to call regex.findall with "cat, bat, sat", "[a-z]at"
print all_matches  # Prints: [cat, bat, sat]
```

**Notes:**

- Built-in functions require exact argument counts
- All math functions require numeric arguments
- String functions require string arguments (except `len` which also works with lists)
- Division by zero returns nil and prints an error
- Function names are reserved and cannot be used as variable names
- File I/O functions will raise errors if files cannot be opened or read
- Path operations work with both absolute and relative paths
- `list_files` excludes `.` and `..` entries
- Regex functions use POSIX Extended Regular Expression syntax
- Invalid regex patterns will raise a `RuntimeError` with details

---

## Functions

Define reusable blocks of code with parameters and return values.

### Function Definition

**Syntax:**

```
function <name> with <param1>, <param2>, ...
    <indented block>
    return <value>
```

**Examples:**

Simple function with one parameter:

```kronos
function greet with name:
    print "Hello,"
    print name
    return name
```

Function with arithmetic:

```kronos
function double with x:
    set result to x times 2
    return result
```

Function with multiple parameters:

```kronos
function add with a, b:
    set sum to a plus b
    return sum
```

Function with multiple statements:

```kronos
function countdown with n:
    print "Counting down from:"
    print n
    set x to n minus 1
    print x
    set y to x minus 1
    print y
    return y
```

### Function Calls

**Syntax:**

```
call <function_name> with <arg1>, <arg2>, ...
```

**Examples:**

Call with one argument:

```kronos
call greet with "Alice"
```

Call with multiple arguments:

```kronos
call add with 5, 3
```

Call with variable arguments:

```kronos
set x to 10
set y to 20
call add with x, y
```

### Return Values

Functions always return a value. If no explicit `return` statement is used, the function returns `nil`.

**Explicit return:**

```kronos
function square with x:
    set result to x times x
    return result
```

**Implicit return (returns nil):**

```kronos
function sayHello with name:
    print "Hello,"
    print name
```

### Local Variables

Variables defined inside a function are local to that function and don't affect global variables with the same name.

```kronos
set x to 100

function test with x:
    print x        # Prints parameter value, not global
    set x to 200   # Modifies local variable
    print x

call test with 50  # Prints 50, then 200
print x            # Prints 100 (global unchanged)
```

### Complete Function Example

```kronos
print "=== Function Demo ==="

function calculateArea with width, height:
    set area to width times height
    print "Calculating area..."
    return area

function greetUser with name, age:
    print "Hello,"
    print name
    print "You are"
    print age
    print "years old"
    return name

call greetUser with "Bob", 25
call calculateArea with 10, 5

print "=== Done ==="
```

### Notes

- Function calls can be used as expressions (can be used in assignments)
- Parameters are passed by value
- All parameters are required (no default values yet)
- Recursive functions are supported
- Functions must be defined before they are called
- Cannot override built-in function names

---

## String Operations

Kronos provides comprehensive string manipulation capabilities including concatenation, indexing, slicing, and built-in functions.

### String Concatenation

Use the `plus` operator to concatenate strings:

```kronos
set greeting to "Hello"
set name to "World"
set message to greeting plus ", " plus name plus "!"
print message  # Output: Hello, World!
```

### String Indexing

Access individual characters using the `at` operator:

```kronos
set text to "Hello"
set first to text at 0      # Returns "H"
set last to text at 4       # Returns "o"
set last_char to text at -1 # Returns "o" (negative index from end)
```

**Notes:**
- Indices start at 0
- Negative indices count from the end (-1 is last character)
- Returns a single-character string

### String Slicing

Extract substrings using the `from ... to` syntax:

```kronos
set text to "Hello World"
set slice1 to text from 0 to 5    # Returns "Hello"
set slice2 to text from 6 to 11   # Returns "World"
set slice3 to text from 0 to end  # Returns "Hello World"
```

**Notes:**
- `from <start> to <end>` extracts characters from start (inclusive) to end (exclusive)
- Use `end` keyword to slice to the end of the string
- Returns a new string

### F-Strings (Formatted String Literals)

F-strings allow embedding expressions inside strings:

```kronos
set name to "Alice"
set age to 30
set greeting to f"Hello, {name}! You are {age} years old."
print greeting  # Output: Hello, Alice! You are 30 years old.

set x to 5
set y to 10
set result to f"{x} plus {y} equals {x plus y}"
print result  # Output: 5 plus 10 equals 15
```

**Features:**
- Syntax: `f"text {expression} more text"` or `f'text {expression}'`
- Expression evaluation inside `{ }` blocks
- Automatic type conversion (numbers, booleans, null → strings)
- Multiple expressions in a single f-string
- Nested expressions and function calls

For complete list of string built-in functions, see [Built-in Constants and Functions](#built-in-constants-and-functions).

---

## Lists and Maps

Kronos provides two collection data types: lists (arrays) and maps (dictionaries).

### Lists

Lists are ordered collections of values. They support indexing, slicing, and iteration.

**Syntax:**
```
list <value1>, <value2>, ...
```

**Examples:**
```kronos
set numbers to list 1, 2, 3, 4, 5
set fruits to list "apple", "banana", "cherry"
set mixed to list 1, "hello", true, 3.14
set empty to list
```

**List Indexing:**
```kronos
set numbers to list 10, 20, 30
set first to numbers at 0      # Returns 10
set last to numbers at -1      # Returns 30 (negative index from end)
```

**List Slicing:**
```kronos
set numbers to list 1, 2, 3, 4, 5
set slice1 to numbers from 1 to 3    # Returns [2, 3]
set slice2 to numbers from 2 to end  # Returns [3, 4, 5]
```

**List Iteration:**
```kronos
set fruits to list "apple", "banana", "cherry"
for fruit in fruits:
    print fruit
```

**List Index Assignment:**
Lists can be modified using `let` with index assignment:
```kronos
let numbers to list 1, 2, 3, 4, 5
let numbers at 0 to 10      # Modify first element: [10, 2, 3, 4, 5]
let numbers at -1 to 50     # Modify last element (negative index): [10, 2, 3, 4, 50]
let numbers at 2 to 5 plus 5  # Modify with expression: [10, 2, 10, 4, 50]
```

**Notes:**
- Only mutable lists (created with `let`) can be modified
- Index must be within bounds (0 to length-1, or negative index from -1 to -length)
- Attempting to modify an immutable list (created with `set`) will result in an error
- Index assignment modifies the list in-place

### Maps

Maps are key-value collections that store pairs of keys and values. Keys can be strings, numbers, booleans, or null.

**Syntax:**
```
map <key1>: <value1>, <key2>: <value2>, ...
```

**Examples:**
```kronos
# Map with string keys
set person to map name: "Alice", age: 30, city: "NYC"

# Map with number keys
set scores to map 1: 100, 2: 200, 3: 300

# Map with boolean keys
set flags to map true: "yes", false: "no"

# Empty map
set empty_map to map

# Mixed value types
set mixed to map key1: "string", key2: 42, key3: true, key4: null
```

**Map Indexing:**
```kronos
set person to map name: "Alice", age: 30
set name to person at "name"    # Returns "Alice"
set age to person at "age"      # Returns 30

# Number keys
set scores to map 1: 100, 2: 200
set score1 to scores at 1       # Returns 100

# Boolean keys
set flags to map true: "yes"
set yes_value to flags at true  # Returns "yes"
```

**Map Key Deletion:**
Map keys can be removed using the `delete` statement:
```kronos
let person to map name: "Alice", age: 30, city: "NYC"
delete person at "age"      # Remove age key: {name: Alice, city: NYC}
delete person at "city"     # Remove city key: {name: Alice}

# Works with any key type
let scores to map 1: 100, 2: 200, 3: 300
delete scores at 2           # Remove key 2: {1: 100, 3: 300}

let flags to map true: "yes", false: "no"
delete flags at true         # Remove boolean key: {false: no}
```

**Notes:**
- Map keys can be any type: strings, numbers, booleans, or null
- Keys in map literals can be identifiers (automatically converted to strings) or expressions
- Accessing a non-existent key results in a runtime error
- Deleting a non-existent key results in a runtime error
- Maps are printed in the format `{key: value, key2: value2}` (order may vary)
- Key deletion modifies the map in-place

**Examples:**
```kronos
# Create a map
set person to map name: "Bob", age: 25

# Access values
print person at "name"    # Prints: Bob
print person at "age"     # Prints: 25

# Map with number keys
set lookup to map 42: "answer", 100: "century"
print lookup at 42        # Prints: answer

# Map with boolean keys
set options to map true: "enabled", false: "disabled"
print options at true     # Prints: enabled
```

---

## Safety & Error Handling

Kronos provides comprehensive safety checks with human-readable error messages and exception handling.

### Error Format

All errors start with `Error:` followed by a clear description:
```
Error: Cannot reassign immutable variable 'x'
Error: Function 'greet' expects 1 argument, but got 2
Error: Cannot divide by zero
```

### Exception Handling

Kronos supports Python-style exception handling with `try`, `catch`, and `finally` blocks. You can raise exceptions with specific error types and catch them selectively.

**Basic Syntax:**
```kronos
try:
    # Code that might raise an exception
    raise "Error message"
catch error:
    # Handle the exception
    print f"Caught: {error}"
finally:
    # Optional cleanup code (always executes)
    print "Cleanup"
```

**Raising Exceptions:**
```kronos
# Raise a generic error
raise "Something went wrong"

# Raise a typed error
raise ValueError "Invalid input value"
raise RuntimeError "Runtime error occurred"
```

**Catching Specific Error Types:**
```kronos
try:
    raise ValueError "Invalid value"
catch ValueError as e:
    print f"Caught ValueError: {e}"
catch RuntimeError as e:
    print f"Caught RuntimeError: {e}"
catch error:
    # Catch-all for any other error type
    print f"Caught other error: {error}"
```

**Multiple Catch Blocks:**
You can have multiple `catch` blocks to handle different error types. The first matching catch block will handle the exception:

```kronos
try:
    raise RuntimeError "Runtime issue"
catch ValueError as e:
    print "This won't catch RuntimeError"
catch RuntimeError as e:
    print f"Caught RuntimeError: {e}"  # This will catch it
catch error:
    print f"Fallback: {error}"
```

**Finally Blocks:**
The `finally` block always executes, whether an exception occurred or not:

```kronos
try:
    raise "error"
catch e:
    print e
finally:
    print "This always runs"
```

**Available Error Types:**
- `RuntimeError` - General runtime errors
- `ValueError` - Invalid argument values
- `TypeError` - Type mismatch errors
- `NameError` - Undefined variable/function errors
- `SyntaxError` - Syntax/parse errors
- `CompileError` - Compilation errors
- `Error` - Generic error (default if no type specified)

### Key Safety Features

✅ **Type Safety** - Operations check types before executing
✅ **Immutability** - `set` variables cannot be reassigned
✅ **Type Annotations** - Optional `as <type>` enforces types
✅ **Function Validation** - Argument count and types checked
✅ **Undefined Detection** - Variables and functions must exist
✅ **Division by Zero** - Caught before execution
✅ **Protected Constants** - Pi cannot be modified
✅ **Exception Handling** - Try/catch/finally for error management

See examples in `tests/fail/` directory for all error cases.

---

## Comments

Comments start with `#` and continue to the end of the line.

**Planned syntax:**

```kronos
# This is a comment
set x to 5  # Comments can also be inline
```

---

## Indentation

Kronos uses indentation to define code blocks, similar to Python.

### Rules

1. **Consistent Indentation:** Use spaces or tabs, but be consistent
2. **Block Indicators:** Colons (`:`) indicate the start of a block
3. **Indent Level:** Increase indentation for nested blocks

### Examples

**Single block:**

```kronos
if x is greater than 5:
    print "x is large"
    print "Still in the if block"
```

**Nested blocks:**

```kronos
for i in range 1 to 3:
    print i
    if i is equal 2:
        print "Found two!"
```

**Multiple nesting:**

```kronos
for i in range 1 to 5:
    print i
    for j in range 1 to 3:
        set product to i times j
        print product
        if product is greater than 10:
            print "Large product"
```

### Common Mistakes

**Incorrect (inconsistent indentation):**

```kronos
if x is greater than 5:
  print "Two spaces"
    print "Four spaces"  # ERROR: Inconsistent
```

**Correct:**

```kronos
if x is greater than 5:
    print "Four spaces"
    print "Four spaces"
```

---

## Complete Examples

### Example 1: Temperature Converter

```kronos
print "Fahrenheit to Celsius Converter"
set fahrenheit to 98.6
set step1 to fahrenheit minus 32
set step2 to step1 times 5
set celsius to step2 divided by 9
print celsius
```

### Example 2: Sum of Numbers

```kronos
print "Sum of numbers 1 to 10:"
set sum to 0
for i in range 1 to 10:
    set sum to sum plus i
print sum
```

### Example 3: Multiplication Table

```kronos
print "Multiplication table for 5:"
for i in range 1 to 10:
    set result to 5 times i
    print result
```

### Example 4: Countdown

```kronos
print "Countdown from 10:"
set count to 10
while count is greater than 0:
    print count
    set count to count minus 1
print "Liftoff!"
```

### Example 5: Find Maximum

```kronos
print "Finding maximum in a sequence"
set max to 0
for i in range 1 to 20:
    if i is greater than max:
        set max to i
print "Maximum is:"
print max
```

---

## Future Features

### Version 0.3.0: "Data Structures & Control Flow" ✅ Completed

**Completed:**
- ✅ Logical operators (`and`, `or`, `not`)
- ✅ Lists/Arrays - Full operations (indexing, slicing, iteration, append, list methods)
- ✅ String operations - Complete manipulation suite (concatenation, indexing, slicing, built-ins)
- ✅ Enhanced standard library - Math, type conversion, list utilities (20+ functions)
- ✅ Control flow - `else if`, `break`, `continue`, range-based loops
- ✅ Range objects - First-class range support

### Version 0.4.0: "Modules & Error Handling" ✅ Completed

**Completed:**
- ✅ Dictionaries/Maps - Key-value storage with full operations
- ✅ Exception Handling - Try/catch/finally blocks with typed exceptions
- ✅ Import/module system - Built-in and file-based modules, namespace management
- ✅ File I/O operations - Complete file system interface (read, write, append, list, path ops)
- ✅ Path operations - join_path, dirname, basename
- ✅ Regular expressions - Pattern matching via regex module

See the [Exception Handling](#exception-handling) section above for complete documentation.

### Version 0.5.0: "Advanced Language Features" (Planned)

**Planned Features:**
- String interpolation - Template strings with expressions and format specifiers
- Multiple return values - Tuple returns and destructuring
- Function enhancements - Default parameters, variadic functions, named arguments
- Anonymous functions / Lambdas - First-class functions, higher-order functions
- List comprehensions - Concise list creation with conditionals
- Pattern matching - Advanced control flow with match expressions
- Type system enhancements - Generic types, type aliases, better inference
- Debugging support - Debug built-in, improved stack traces

### Version 1.0.0: "Production Release" (Planned)

**Planned Features:**
- Concurrency - Goroutines and channels with `spawn`, `send`, `receive`, `select`, worker pools
- Complete standard library - 50+ functions (math, string, date/time, collections, JSON, system)
- Method chaining - Fluent API support
- Performance optimizations - Bytecode optimization, constant folding, inline caching
- Standard library modules - `math`, `string`, `os`, `json`, `time`, `collections`, `regex`
- Tooling - Package manager, formatter, linter, test runner, documentation generator

**Example:**
```kronos
# Concurrency (planned)
spawn task with:
    print "Running in parallel"
```

---

## Tips and Best Practices

1. **Use descriptive variable names:**

   ```kronos
   # Good
   set totalPrice to 100
   set customerName to "John"

   # Avoid
   set x to 100
   set n to "John"
   ```

2. **Break complex expressions into steps:**

   ```kronos
   # Instead of nested calculations
   set x to a plus b times c minus d divided by e

   # Break it down
   set product to b times c
   set quotient to d divided by e
   set sum to a plus product
   set result to sum minus quotient
   ```

3. **Consistent indentation (4 spaces recommended):**

   ```kronos
   if condition:
       print "Use 4 spaces"
       for i in range 1 to 5:
           print i
   ```

4. **Initialize counters before loops:**

   ```kronos
   set sum to 0  # Initialize before use
   for i in range 1 to 10:
       set sum to sum plus i
   ```

5. **Be careful with infinite loops:**

   ```kronos
   # Good - counter increases
   set count to 0
   while count is less than 10:
       print count
       set count to count plus 1

   # Bad - infinite loop
   set count to 0
   while count is less than 10:
       print count
       # Forgot to increment!
   ```

---

## Error Messages

Common errors you might encounter:

- **"Undefined variable"** - Using a variable before setting it
- **"Division by zero"** - Attempting to divide by zero
- **"Stack overflow"** - Too many nested operations
- **"Unexpected token"** - Syntax error in your code

---

## Getting Help

For more examples, check the `examples/` directory:

- `hello.kr` - Basic printing and variables
- `arithmetic.kr` - Arithmetic operations
- `conditionals.kr` - If statements
- `loops.kr` - For and while loops
- `test.kr` - Mixed examples

Run the REPL for interactive testing:

```bash
./kronos
```

Execute a file:

```bash
./kronos your_program.kr
```

---

_Last updated: December 2025_
_Kronos Language Version: 0.4.0_
