# Commit Message Conventions

This document outlines the commit message conventions used in the Kronos project.

## Format

All commit messages should follow this format:

```
(Type): description

Optional body with more details
```

## Commit Types

Use one of the following types in parentheses:

- **`(Feat)`**: New features or functionality
- **`(Fix)`**: Bug fixes
- **`(Docs)`**: Documentation changes (README, docs/, comments)
- **`(Test)`**: Adding or modifying tests
- **`(Refactor)`**: Code refactoring without changing functionality
- **`(Chore)`**: Maintenance tasks, build system changes, dependencies

## Examples

### Feature Addition

```
(Feat): add exception handling with try/catch/finally and typed exceptions (#16)
```

### Bug Fix

```
(Fix): error handling in `vm_load_module` to ensure proper memory management and error reporting for module file reads
```

### Documentation

```
(Docs): expand exception handling section in SYNTAX.md and QUICKREF.md with detailed examples and explanations for try/catch/finally blocks, raising exceptions, and handling specific error types
```

### Tests

```
(Test): add integration tests for map key deletion and list index assignment scenarios
```

### Refactoring

```
(Refactor): enhance value release logic in `vm_execute` by removing redundant release calls for iterable, improving memory management
```

### Chores

```
(Chore): define `_POSIX_C_SOURCE` for compatibility with `POSIX` standards
```

## Guidelines

1. **Use imperative mood**: Write "add feature" not "added feature" or "adds feature"
2. **Keep it concise**: The first line should be a brief summary (50-72 characters recommended)
3. **Be descriptive**: Clearly describe what the commit does
4. **Reference issues/PRs**: Include issue or PR numbers when applicable: `(#16)`
5. **Use lowercase**: Start the description with lowercase (unless it's a proper noun)
6. **Add body if needed**: For complex changes, add a blank line and provide more details

## Multi-line Commits

For more complex changes, use a multi-line format:

```
(Feat): implement file-based module system with LSP support (#13)

- Add module loading and management in VM
- Enhance document state to support imported modules
- Implement `OP_IMPORT` instruction for module imports
- Add current file path management for relative imports
```

## Breaking Changes

If your commit introduces a breaking change, indicate it clearly:

```
(Feat): change function signature for `value_new_string`

BREAKING CHANGE: `value_new_string` now requires a length parameter
```

## Why These Conventions?

- **Consistency**: Makes it easy to scan commit history
- **Automation**: Enables automated changelog generation
- **Clarity**: Quickly understand what each commit does
- **Filtering**: Easy to filter commits by type (e.g., `git log --grep="(Feat)"`)

## Tools

You can use these git aliases to filter commits by type:

```bash
# View only features
git log --grep="(Feat)"

# View only bug fixes
git log --grep="(Fix)"

# View only documentation changes
git log --grep="(Docs)"
```

