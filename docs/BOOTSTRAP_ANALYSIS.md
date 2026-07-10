# Kronos Bootstrap Analysis

## What "Bootstrap" Means

This document analyzes when Kronos can reasonably be said to "build itself."

There are two useful milestones:

1. **Hosted self-compiler**

   A compiler written in Kronos that runs on top of the current C implementation.

2. **Autonomous bootstrap**

   A Kronos implementation that can produce and reuse its own compiler artifacts
   without depending on the C host frontend for each iteration.

These are different goals. With the current language surface, Kronos is now
comfortably in range of the first milestone. It still needs an artifact path or
native build orchestration to reach the second.

## Current State

Kronos is currently tracked as **0.5.2-beta**, with the website runtime migration
complete.

The implementation remains centered on a C toolchain:

- Tokenizer in C
- Parser in C
- Compiler in C
- Bytecode VM in C
- Built-in runtime/library modules in C
- LSP server in C
- WASM build of the same runtime for the website playground

Kronos compiles source to bytecode and executes that bytecode in the VM. The
current architecture is bytecode-first; there is still no C code generation
backend in the repo.

## What Kronos Already Has

The available language and runtime surface is now broad enough for a substantial
compiler implementation in Kronos itself:

- Strings, string indexing/slicing, f-strings, and string helpers
- Lists, list indexing/slicing, list mutation, comprehensions, and higher-order
  helpers (`map`, `filter`, `sort`, `reverse`, `zip`, `enumerate`, `any`, `all`,
  `sum`)
- Maps/dictionaries with hash-table storage, literals, indexing, mutation, and
  deletion
- Ranges with indexing, slicing, iteration, and length
- Functions with parameters, return values, default parameters, variadic
  parameters, named arguments, lambdas, and multiple return values
- Control flow (`if`, `else if`, `else`, `for`, `while`, `break`, `continue`,
  `match`)
- Arithmetic, comparison, logical operators, modulo, and unary negation
- Type annotations, generic types, type aliases, and union types
- Modules (`import math`, other built-in modules, and `import module from
  "file.kr"`)
- Exceptions (`try` / `catch` / `finally`, typed raises, catch-all handling)
- File I/O (`read_file`, `write_file`, `read_lines`)
- File/path helpers (`file_exists`, `list_files`, `join_path`, `dirname`,
  `basename`)
- Regular expressions (`regex.match`, `regex.search`, `regex.findall`)
- JSON helpers (`json.parse_json`, `json.to_json`)
- Time and OS-adjacent helpers (`time.now`, `time.sleep`, date formatting/parsing,
  `os.args`, `os.env`, `os.exit`)
- Debug statements and LSP coverage for a large part of the language

From a language-expressiveness standpoint, this is enough to write:

- A tokenizer
- A parser
- AST and IR transforms
- Semantic checks
- A bytecode assembler/emitter
- A source-to-source tool or formatter
- Compiler-driver logic that reads source files and writes generated artifacts

The main hosted-compiler work is now engineering effort and specification
discipline, not waiting for one missing collection type or syntax feature.

## What Is Still Missing

### Critical for Autonomous Bootstrap

1. **Bytecode/artifact serialization and loading**

   The C compiler produces a `Bytecode` structure internally, and functions carry
   bytecode bodies at runtime, but the repo does not expose a stable bytecode
   file format or user-facing save/load/execute path.

   Why this matters:

   - A self-hosted compiler needs to emit a reusable artifact.
   - The runtime needs to load that artifact later without sending source through
     the C tokenizer, parser, and compiler.
   - The artifact format needs to encode instructions, constants, function
     metadata, module references, and enough versioning to survive VM changes.

2. **System/process execution**

   Kronos now has `os.args`, `os.env`, and `os.exit`, but no language-level
   `system()`, `exec()`, `spawn()`, or subprocess API.

   Why this matters:

   - A Kronos compiler that emits C would need to invoke `gcc`/`clang`.
   - A toolchain written in Kronos eventually needs to run external build,
     testing, packaging, and documentation steps.
   - Without process execution, Kronos can still generate files, but another
     host must orchestrate non-Kronos commands.

### Important but Not Strictly Required for a First Hosted Compiler

3. **A formal bytecode/IR specification**

   A hosted compiler can start by matching the current C compiler's bytecode, but
   that bytecode is currently a C-internal contract. A stable spec would reduce
   the risk of the Kronos compiler chasing implementation details.

4. **Binary data or byte-oriented file APIs**

   The current file APIs are enough for text source, JSON, and textual assembly
   formats. A compact bytecode artifact would be easier with byte arrays or
   explicit binary read/write support. This is not required if the first artifact
   format is textual.

5. **More self-hosted tooling**

   Formatter, linter, test runner, package tooling, and compiler fixture tooling
   would make a self-hosted compiler maintainable, but they are not blockers for
   the first hosted compiler.

### Explicitly Not a Blocker

6. **Concurrency**

   Concurrency may help build throughput later, but it is not needed for
   bootstrap. The parser, compiler, and bytecode emitter can be single-threaded.

## Bootstrap Strategy Options

### Option 1: Hosted Self-Compiler Targeting the Existing Bytecode Model

**Approach:** Write the compiler in Kronos and have it run on the current C
VM/runtime.

- Read `.kr` source files.
- Tokenize and parse in Kronos.
- Perform semantic analysis.
- Emit an IR or textual bytecode assembly aligned with the existing VM.
- Use the current C implementation to run and validate the compiler.

**Pros:**

- Matches the current architecture.
- Uses language features that already exist.
- Avoids adding a native backend before compiler logic exists.
- Creates direct pressure to specify the VM bytecode contract.

**Cons:**

- Still depends on the current C implementation as the host.
- Does not by itself produce a reusable standalone compiler artifact.
- Needs either a bytecode assembler/load path or a textual intermediate format
  consumed by C tooling.

**Assessment:** This is still the best next bootstrap milestone, and it is more
plausible now than the older analysis suggested.

### Option 2: Bytecode Serialization / Compiler Artifact Path

**Approach:** Extend Kronos and the C host so compiled artifacts can be saved,
loaded, and executed as a stable format.

- Specify the bytecode artifact format.
- Add C-side save/load/execute support.
- Let the Kronos compiler emit that format.
- Load the emitted artifact later without recompiling source through the C
  frontend.

**Pros:**

- Fits the bytecode-first implementation.
- Avoids a C backend as the primary route to bootstrap.
- Provides the cleanest route from hosted compiler to autonomous bootstrap.
- Benefits distribution and startup time independently of self-hosting.

**Cons:**

- Requires a stable bytecode/versioning contract.
- Needs serialization for constants, functions/lambdas, module metadata, and
  possibly debug/source mapping data.
- Requires careful compatibility handling as opcodes evolve.

**Assessment:** This is the preferred autonomous-bootstrap path.

### Option 3: Add a C Code Generation Backend

**Approach:** Write a compiler in Kronos that emits C, then invoke a system
compiler.

- Read `.kr` source files.
- Parse and analyze in Kronos.
- Generate C code.
- Call `gcc` or `clang`.

**Pros:**

- Leverages mature native toolchains.
- Produces conventional binaries.
- Could eventually support native distribution.

**Cons:**

- Does not match the current compiler architecture.
- Requires a new C backend, runtime ABI design, and process execution support.
- Larger scope than the bytecode-artifact path.
- Risks duplicating VM semantics in generated C before the language spec is
  stable.

**Assessment:** Viable later, but no longer the default bootstrap recommendation.

### Option 4: Textual Bytecode Assembly as an Intermediate Step

**Approach:** Introduce a documented text representation of bytecode that Kronos
can generate using existing string and file APIs, then add a C-side assembler or
loader for that representation.

**Pros:**

- Avoids needing binary file APIs immediately.
- Easier to inspect, diff, and test while the bytecode format settles.
- Gives the self-hosted compiler a concrete artifact target earlier.

**Cons:**

- Adds an intermediate format that may be temporary.
- Still needs a C-side loader/assembler before artifacts are reusable.
- Less compact and likely slower to load than binary bytecode.

**Assessment:** A pragmatic bridge if binary bytecode serialization feels too
large for the first artifact milestone.

## Recommended Path

The most practical roadmap is:

1. Define the compiler boundary: AST-to-bytecode parity, textual bytecode
   assembly, or direct bytecode artifact emission.
2. Build a hosted tokenizer/parser in Kronos with golden tests against the C
   frontend.
3. Add semantic analysis and bytecode/IR emission in Kronos.
4. Add bytecode/artifact serialization and a C-side load/execute path.
5. Use that artifact path to make the self-hosted compiler reusable without the
   C frontend.
6. Treat C code generation and subprocess orchestration as optional later
   backend/tooling work.

This sequencing matches the current repo better than making C code generation
the first bootstrap target.

## Milestone Estimate

### Stage 1: Hosted Self-Compiler

**Status:** Feasible on the current language/runtime surface.

Kronos has the syntax, collections, file I/O, modules, exceptions, JSON, and
higher-order features needed to implement compiler logic. The risk is now mostly
scope, correctness, and test coverage.

Suggested first milestone:

- Kronos tokenizer and parser for a deliberately chosen language subset.
- AST represented as maps/lists or a small tagged-node convention.
- Golden tests comparing Kronos-produced tokens/AST summaries with the C
  frontend.
- No bytecode emission required yet.

### Stage 2: Hosted Compiler That Emits VM-Compatible Artifacts

**Status:** Plausible, but needs an artifact decision.

The next useful target is either:

- A textual bytecode assembly file, plus a C assembler/loader; or
- A direct serialized bytecode format, plus a C loader.

This stage turns the self-hosted compiler from an analysis tool into a real
compiler.

### Stage 3: Autonomous Bootstrap

**Status:** Still blocked.

The missing pieces are not parser features or collection types. The blockers are:

- No user-facing bytecode/artifact persistence and load path.
- No language-level process execution for a C-emitting or native-build path.

Either of these could unblock a route:

- **Bytecode/artifact persistence** for a bytecode-first bootstrap.
- **Process execution** for a C-emitting bootstrap.

The bytecode-artifact route remains the better fit for the existing codebase.

## Current Implementation Size

Current rough implementation size in the repo:

- `main.c`: 1,024 LOC
- `linenoise.c`: 1,353 LOC
- `src/core/runtime.c`: 2,119 LOC
- `src/core/gc.c`: 882 LOC
- `src/frontend/tokenizer.c`: 1,169 LOC
- `src/frontend/parser.c`: 6,350 LOC
- `src/compiler/compiler.c`: 4,301 LOC
- `src/vm/vm.c`: 5,787 LOC
- `src/lsp/*.c`: 12,386 LOC combined
- `wasm/kronos_wasm.c`: 336 LOC

**Measured total across those files:** 35,707 LOC

There are also over 100 passing integration programs, over 60 failing/error
integration programs, unit tests, LSP tests, a website, and a WASM runtime path.
The self-hosting effort should therefore be planned as an incremental compiler
project with compatibility gates, not a one-shot rewrite.

## Conclusion

**Hosted self-compiler:** Feasible now on the current 0.5.x language surface.

**Autonomous bootstrap:** Still blocked until Kronos can either load reusable
compiler artifacts or orchestrate an external native toolchain.

**What is no longer true:** Kronos is not waiting on maps, modules, file I/O,
exceptions, lambdas, comprehensions, pattern matching, or enhanced type syntax.
Those are already implemented.

**Real remaining blockers for autonomous bootstrap:**

1. Stable bytecode/artifact serialization and loading.
2. System/process execution support, if the chosen path emits C or coordinates
   external build steps.

**Recommended strategy:** Target the existing bytecode-oriented architecture
first. Add a textual or binary artifact path before investing in C code
generation as the primary bootstrap mechanism.
