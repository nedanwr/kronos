# Kronos Bootstrap Analysis

## What "Bootstrap" Means

This document analyzes when Kronos can reasonably be said to "build itself."

There are two useful milestones:

1. **Hosted self-compiler**

   A compiler written in Kronos that runs on top of the current C implementation.

2. **Autonomous bootstrap**

   A Kronos implementation that can produce and reuse its own compiler artifacts without depending on the C host build for each iteration.

These are different goals. Kronos is much closer to the first than the second.

## Current State

Kronos is currently in **public beta** at **v0.4.5**.

The implementation is still centered on a C toolchain:

- Tokenizer in C
- Parser in C
- Compiler in C
- Bytecode VM in C
- WASM build of the same runtime for the website playground

Kronos already compiles source to bytecode and executes that bytecode in the VM. There is no C code generation backend in the current architecture.

## What Kronos Already Has

These features are already present and are sufficient for implementing a substantial amount of compiler logic in Kronos itself:

- ✅ Strings and string manipulation
- ✅ Lists
- ✅ Maps / dictionaries
- ✅ Ranges
- ✅ Functions
- ✅ Control flow (`if`, `for`, `while`, `break`, `continue`)
- ✅ Arithmetic, comparison, and logical operators
- ✅ Modules (`import math`, `import regex`, `import module from "file.kr"`)
- ✅ Exceptions (`try` / `catch` / `finally`)
- ✅ File I/O (`read_file`, `write_file`, `read_lines`)
- ✅ File/path helpers (`file_exists`, `list_files`, `join_path`, `dirname`, `basename`)
- ✅ Regular expressions (`regex.match`, `regex.search`, `regex.findall`)

From a language-expressiveness standpoint, this is enough to write:

- A tokenizer
- A parser
- AST transforms
- Semantic checks
- A simple code generator or bytecode emitter

## What Is Still Missing

### Critical for Autonomous Bootstrap

1. **Artifact generation and reuse**

   Kronos can generate bytecode internally today, but the repo does not currently expose bytecode save/load or serialization as a user-facing capability.

   Why this matters:

   - A self-hosted compiler needs a way to emit a reusable build artifact
   - That artifact must be loadable later without recompiling from source through the C host

2. **System / process execution**

   There is no language-level `system()`, `exec()`, or similar process API in the current implementation.

   Why this matters:

   - A Kronos compiler that emits C would need to invoke `gcc`/`clang`
   - A toolchain written in Kronos eventually needs some way to orchestrate external build steps

### Important but Not Strictly Required for a First Hosted Compiler

3. **Better tooling and packaging**

   A self-hosted compiler is easier to maintain once formatter/linter/test tooling exists in Kronos itself. These are roadmap items, not blockers for a first compiler implementation.

4. **Advanced language features**

   v0.5.0 is focused on lambdas, list comprehensions, pattern matching, and an enhanced type system. These will help ergonomics, but they are not required for a basic compiler.

### Explicitly Not a Blocker

5. **Concurrency**

   Concurrency is planned for **v0.8.0**. It may help performance later, but it is not needed for bootstrap.

## Bootstrap Strategy Options

### Option 1: Hosted Self-Compiler Targeting Existing Bytecode Model

**Approach:** Write the compiler in Kronos and have it run on the current C VM/runtime.

- Read `.kr` source files
- Tokenize and parse in Kronos
- Perform semantic analysis
- Emit an internal representation aligned with the existing bytecode VM
- Use the current host implementation to run and validate the compiler

**Pros:**

- Matches the current architecture
- Uses language features that already exist
- Avoids adding a new backend before the compiler logic exists

**Cons:**

- Still depends on the current C implementation as the host
- Does not by itself produce a reusable standalone compiler artifact

**Assessment:** This is the most realistic next bootstrap milestone.

### Option 2: Bytecode Serialization / Compiler Artifact Path

**Approach:** Extend Kronos so compiled bytecode can be saved, loaded, and executed as a stable artifact format.

- Write compiler in Kronos
- Emit bytecode or a serialized compiler artifact
- Save artifact to disk
- Load artifact later without recompiling from source through the C frontend

**Pros:**

- Fits the current bytecode-first implementation
- Reduces dependency on an added C backend
- Moves Kronos toward genuine autonomous bootstrap

**Cons:**

- Requires a stable bytecode or artifact format
- Needs explicit serialization/loading support that does not exist yet

**Assessment:** This is the cleanest path to an actual autonomous bootstrap.

### Option 3: Add a C Code Generation Backend

**Approach:** Write a compiler in Kronos that emits C, then invoke a system compiler.

- Read `.kr` source files
- Parse and analyze in Kronos
- Generate C code
- Call `gcc` or `clang`

**Pros:**

- Leverages mature native toolchains
- Produces conventional binaries

**Cons:**

- Does not match the current compiler architecture
- Requires both a new C backend and system/process execution support
- Larger scope than the bytecode-artifact path

**Assessment:** Viable, but no longer the default recommendation.

## Recommended Path

The most practical roadmap is:

1. Build a **hosted self-compiler** in Kronos first
2. Add **bytecode/artifact serialization**
3. Use that artifact path to reach **autonomous bootstrap**
4. Treat C code generation as an optional later backend, not the first bootstrap target

This sequencing matches the current repo better than the older "generate C first" framing.

## Milestone Estimate

### Stage 1: Hosted Self-Compiler

**Status:** Plausible now on the current language/runtime surface

Kronos already has the core language features needed to implement compiler logic. The main work is engineering effort, not waiting on one missing syntax feature.

### Stage 2: Autonomous Bootstrap

**Status:** Still blocked

The missing pieces are not parser features or collection types. The blockers are:

- No user-facing bytecode/artifact persistence
- No language-level process execution

Either of these could unblock a path:

- **Bytecode/artifact persistence** for a bytecode-first bootstrap
- **Process execution** for a C-emitting bootstrap

## Current Implementation Size

The older compiler-size estimate is no longer accurate.

Current rough implementation size in the repo:

- `main.c`: ~1,013 LOC
- `linenoise.c`: ~1,353 LOC
- `src/core/runtime.c`: ~1,774 LOC
- `src/core/gc.c`: ~848 LOC
- `src/frontend/tokenizer.c`: ~1,143 LOC
- `src/frontend/parser.c`: ~5,498 LOC
- `src/compiler/compiler.c`: ~3,844 LOC
- `src/vm/vm.c`: ~8,104 LOC
- `src/lsp/*.c`: ~7,515 LOC combined
- `wasm/kronos_wasm.c`: ~336 LOC

**Measured total across those files:** ~31,551 LOC

That means a self-hosted implementation effort is now large enough that bootstrap planning should focus on architecture and artifact flow, not just language feature checklisting.

## Conclusion

**Earliest realistic bootstrap point:** A **hosted self-compiler** is likely feasible now or soon on the current `v0.4.x` language surface.

**What is no longer true:** Kronos is not mainly waiting on maps, modules, file I/O, or exceptions. Those are already implemented.

**Real remaining blockers for autonomous bootstrap:**

1. Bytecode/artifact serialization and loading
2. System/process execution support

**Recommended strategy:** Target the existing bytecode-oriented architecture first, then add artifact persistence. That is a better fit for the repo than making C code generation the primary bootstrap plan.
