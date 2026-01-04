"use client";

import { useState, useMemo, useEffect, useCallback, useRef } from "react";
import {
  Play,
  RotateCcw,
  Copy,
  Check,
  ChevronDown,
  Terminal,
  Sparkles,
  Clock,
  Loader2,
  AlertCircle,
} from "lucide-react";
import Link from "next/link";

import { Button } from "~/components/ui/button";
import { highlightKronos } from "./_syntax-highlighter";
import { getKronosRuntime, type KronosRuntime } from "~/lib/kronos-wasm";

const examples = [
  {
    name: "Hello World",
    code: `# Your first Kronos program
set message to "Hello, World!"
print message`,
  },
  {
    name: "Variables",
    code: `# Immutable variable
set name to "Kronos"

# Mutable variable
let counter to 0
set counter to counter plus 1

print f"Name: {name}"
print f"Counter: {counter}"`,
  },
  {
    name: "Functions",
    code: `# Define a function
function greet with name:
    return f"Hello, {name}!"

# Call the function
set message to call greet with "Developer"
print message

# Function with multiple parameters
function add with a, b:
    return a plus b

print call add with 5, 3`,
  },
  {
    name: "Loops",
    code: `# For loop with range
print "Counting to 5:"
for i in range 1 to 6:
    print i

# For loop with list
set fruits to list "apple", "banana", "cherry"
print "\\nFruits:"
for fruit in fruits:
    print f"  - {fruit}"

# While loop
let n to 3
print "\\nCountdown:"
while n is greater than 0:
    print n
    set n to n minus 1
print "Blast off!"`,
  },
  {
    name: "Lists & Maps",
    code: `# Create a list
set numbers to list 1, 2, 3, 4, 5
print f"Numbers: {numbers}"
print f"First: {numbers at 0}"
print f"Length: {call len with numbers}"

# Create a map
set user to map "name": "Alice", "age": 30
print f"\\nUser: {user}"
print f"Name: {user at \\"name\\"}"`,
  },
  {
    name: "Error Handling",
    code: `# Try-catch-finally
function divide with a, b:
    if b is equal 0:
        raise "Cannot divide by zero"
    return a divided by b

try:
    set result to call divide with 10, 2
    print f"10 / 2 = {result}"
    
    set result to call divide with 10, 0
    print f"This won't print"
catch error:
    print f"Error: {error}"
finally:
    print "Calculation complete"`,
  },
  {
    name: "FizzBuzz",
    code: `# Classic FizzBuzz
for i in range 1 to 16:
    if i modulo 15 is equal 0:
        print "FizzBuzz"
    else:
        if i modulo 3 is equal 0:
            print "Fizz"
        else:
            if i modulo 5 is equal 0:
                print "Buzz"
            else:
                print i`,
  },
];

export default function PlaygroundPage() {
  const [code, setCode] = useState(examples[0].code);
  const [output, setOutput] = useState("");
  const [isRunning, setIsRunning] = useState(false);
  const [copied, setCopied] = useState(false);
  const [selectedExample, setSelectedExample] = useState(examples[0].name);
  const [showExamples, setShowExamples] = useState(false);
  const [wasmStatus, setWasmStatus] = useState<
    "loading" | "ready" | "error" | "unavailable"
  >("loading");
  const [wasmError, setWasmError] = useState<string | null>(null);
  const runtimeRef = useRef<KronosRuntime | null>(null);

  // Memoized syntax highlighting
  const highlightedCode = useMemo(() => highlightKronos(code), [code]);

  // Initialize WASM runtime on mount
  useEffect(() => {
    const initRuntime = async () => {
      try {
        const runtime = getKronosRuntime();
        await runtime.initialize();
        runtimeRef.current = runtime;
        setWasmStatus("ready");
      } catch (error) {
        console.error("Failed to initialize Kronos WASM:", error);
        setWasmStatus("unavailable");
        setWasmError("WASM module not available. Build with: make wasm");
      }
    };

    initRuntime();

    // Cleanup on unmount
    return () => {
      // Don't cleanup the singleton - it can be reused
    };
  }, []);

  const handleRun = useCallback(async () => {
    if (wasmStatus !== "ready" || !runtimeRef.current) {
      // Show fallback message if WASM not available
      setOutput(
        `⏱ Kronos Playground\n` +
          `━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n` +
          `⚠️ WASM runtime not available.\n\n` +
          `To enable execution, build the WASM module:\n` +
          `  1. Install Emscripten SDK\n` +
          `  2. Run: make wasm\n\n` +
          `For now, install Kronos locally:\n` +
          `  git clone https://github.com/nedanwr/kronos\n` +
          `  cd kronos && make\n` +
          `  ./kronos your_file.kr\n\n` +
          `━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━`
      );
      return;
    }

    setIsRunning(true);
    setOutput("");

    try {
      const result = await runtimeRef.current.run(code);

      if (result.success) {
        setOutput(result.output || "(no output)");
      } else {
        setOutput(result.error || "Unknown error occurred");
      }
    } catch (error) {
      setOutput(
        `Error: ${error instanceof Error ? error.message : "Execution failed"}`
      );
    } finally {
      setIsRunning(false);
    }
  }, [code, wasmStatus]);

  // Keyboard shortcut: Cmd/Ctrl + Enter to run
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
        e.preventDefault();
        handleRun();
      }
    };

    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [handleRun]);

  const handleCopy = async () => {
    await navigator.clipboard.writeText(code);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  const handleReset = () => {
    const example = examples.find((e) => e.name === selectedExample);
    if (example) {
      setCode(example.code);
    }
    setOutput("");
  };

  const handleExampleSelect = (exampleName: string) => {
    const example = examples.find((e) => e.name === exampleName);
    if (example) {
      setCode(example.code);
      setSelectedExample(exampleName);
      setOutput("");
    }
    setShowExamples(false);
  };

  return (
    <div className="relative min-h-screen overflow-hidden bg-[#050505]">
      {/* Background effects */}
      <div className="pointer-events-none fixed inset-0 overflow-hidden">
        <div className="absolute -right-1/4 -top-1/4 h-[800px] w-[800px] rounded-full bg-linear-to-bl from-[#2E1065]/60 via-[#6D28D9]/20 to-transparent blur-3xl opacity-30 sm:opacity-50 lg:opacity-100" />
        <div className="absolute -bottom-1/4 -left-1/4 h-[700px] w-[700px] rounded-full bg-linear-to-tr from-[#1E1B4B]/50 via-[#2E1065]/30 to-transparent blur-3xl opacity-30 sm:opacity-50 lg:opacity-100" />
        <div className="absolute inset-0 bg-[url('data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIzMDAiIGhlaWdodD0iMzAwIj48ZmlsdGVyIGlkPSJhIiB4PSIwIiB5PSIwIj48ZmVUdXJidWxlbmNlIGJhc2VGcmVxdWVuY3k9Ii43NSIgc3RpdGNoVGlsZXM9InN0aXRjaCIgdHlwZT0iZnJhY3RhbE5vaXNlIi8+PGZlQ29sb3JNYXRyaXggdHlwZT0ic2F0dXJhdGUiIHZhbHVlcz0iMCIvPjwvZmlsdGVyPjxyZWN0IHdpZHRoPSIxMDAlIiBoZWlnaHQ9IjEwMCUiIGZpbHRlcj0idXJsKCNhKSIgb3BhY2l0eT0iMC4wNSIvPjwvc3ZnPg==')] opacity-50" />
      </div>

      <div className="relative z-10">
        {/* Header */}
        <header className="border-b border-white/10 bg-[#050505]/80 backdrop-blur-xl">
          <div className="mx-auto flex h-14 max-w-[1800px] items-center justify-between px-4">
            <div className="flex items-center gap-4">
              <Link
                href="/"
                className="flex items-center gap-2 text-lg font-semibold"
              >
                <span className="text-[#F59E0B]">⏱</span>
                <span className="text-white">Kronos</span>
              </Link>
              <span className="text-white/30">/</span>
              <div className="flex items-center gap-2">
                <Sparkles className="h-4 w-4 text-[#F59E0B]" />
                <span className="font-medium text-white">Playground</span>
              </div>
            </div>

            <div className="flex items-center gap-3">
              <Link href="/docs">
                <Button
                  variant="ghost"
                  size="sm"
                  className="text-white/60 hover:bg-white/5 hover:text-white"
                >
                  Docs
                </Button>
              </Link>
              <a
                href="https://github.com/nedanwr/kronos"
                target="_blank"
                rel="noopener noreferrer"
              >
                <Button
                  variant="ghost"
                  size="sm"
                  className="text-white/60 hover:bg-white/5 hover:text-white"
                >
                  GitHub
                </Button>
              </a>
            </div>
          </div>
        </header>

        {/* Toolbar */}
        <div className="relative z-20 border-b border-white/10 bg-[#0a0a0a]/80 backdrop-blur-sm">
          <div className="mx-auto flex h-12 max-w-[1800px] items-center justify-between px-4">
            <div className="flex items-center gap-3">
              {/* Example selector */}
              <div className="relative">
                <button
                  onClick={() => setShowExamples(!showExamples)}
                  className="flex items-center gap-2 rounded-lg border border-white/10 bg-white/5 px-3 py-1.5 text-sm text-white/80 transition-colors hover:bg-white/10"
                >
                  <span>{selectedExample}</span>
                  <ChevronDown
                    className={`h-4 w-4 transition-transform ${
                      showExamples ? "rotate-180" : ""
                    }`}
                  />
                </button>

                {showExamples && (
                  <div className="absolute left-0 top-full z-100 mt-1 w-48 rounded-lg border border-white/10 bg-[#0a0a0a] py-1 shadow-xl">
                    {examples.map((example) => (
                      <button
                        key={example.name}
                        onClick={() => handleExampleSelect(example.name)}
                        className={`w-full px-3 py-2 text-left text-sm transition-colors hover:bg-white/5 ${
                          selectedExample === example.name
                            ? "bg-[#F59E0B]/10 text-[#F59E0B]"
                            : "text-white/70"
                        }`}
                      >
                        {example.name}
                      </button>
                    ))}
                  </div>
                )}
              </div>
            </div>

            <div className="flex items-center gap-2">
              <Button
                variant="ghost"
                size="sm"
                onClick={handleCopy}
                className="gap-2 text-white/60 hover:bg-white/5 hover:text-white"
              >
                {copied ? (
                  <Check className="h-4 w-4 text-green-400" />
                ) : (
                  <Copy className="h-4 w-4" />
                )}
                <span className="hidden sm:inline">
                  {copied ? "Copied!" : "Copy"}
                </span>
              </Button>
              <Button
                variant="ghost"
                size="sm"
                onClick={handleReset}
                className="gap-2 text-white/60 hover:bg-white/5 hover:text-white"
              >
                <RotateCcw className="h-4 w-4" />
                <span className="hidden sm:inline">Reset</span>
              </Button>
              <Button
                size="sm"
                onClick={handleRun}
                disabled={isRunning}
                className="gap-2 border border-[#F59E0B]/50 bg-[rgba(245,158,11,0.15)] text-[#F59E0B] hover:border-[#F59E0B] hover:bg-[rgba(245,158,11,0.25)]"
              >
                {isRunning ? (
                  <Clock className="h-4 w-4 animate-spin" />
                ) : (
                  <Play className="h-4 w-4" />
                )}
                <span>{isRunning ? "Running..." : "Run"}</span>
              </Button>
            </div>
          </div>
        </div>

        {/* Main content */}
        <div
          className="mx-auto max-w-[1800px] p-4"
          onClick={() => setShowExamples(false)}
        >
          <div className="grid h-[calc(100vh-180px)] gap-4 lg:grid-cols-2">
            {/* Editor Panel */}
            <div className="flex flex-col overflow-hidden rounded-xl border border-white/10 bg-[#0a0a0a]/80 backdrop-blur-sm">
              <div className="flex items-center gap-2 border-b border-white/10 bg-white/5 px-4 py-2">
                <div className="flex gap-1.5">
                  <div className="h-3 w-3 rounded-full bg-red-500/80" />
                  <div className="h-3 w-3 rounded-full bg-yellow-500/80" />
                  <div className="h-3 w-3 rounded-full bg-green-500/80" />
                </div>
                <span className="ml-2 text-sm text-white/40">main.kr</span>
              </div>
              <div className="relative flex-1 overflow-auto">
                {/* Line numbers gutter */}
                <div
                  className="pointer-events-none absolute left-0 top-0 bottom-0 w-12 select-none border-r border-white/5 bg-white/2 py-4 font-mono text-sm leading-relaxed text-white/30"
                  aria-hidden="true"
                  style={{ minHeight: "100%" }}
                >
                  {code.split("\n").map((_, i) => (
                    <div key={i} className="px-2 text-right h-6.5">
                      {i + 1}
                    </div>
                  ))}
                </div>
                {/* Syntax highlighted code overlay */}
                <div
                  className="pointer-events-none absolute inset-0 py-4 pl-14 pr-4 font-mono text-sm leading-relaxed"
                  aria-hidden="true"
                  style={{ tabSize: 4 }}
                >
                  {highlightedCode}
                </div>
                {/* Code editor (transparent text, handles input) */}
                <textarea
                  value={code}
                  onChange={(e) => setCode(e.target.value)}
                  onKeyDown={(e) => {
                    const textarea = e.currentTarget;
                    const start = textarea.selectionStart;
                    const end = textarea.selectionEnd;
                    const indent = "    "; // 4 spaces

                    if (e.key === "Tab") {
                      e.preventDefault();
                      // Insert indent at cursor position
                      const newCode =
                        code.substring(0, start) + indent + code.substring(end);
                      setCode(newCode);

                      // Move cursor after the inserted indent
                      requestAnimationFrame(() => {
                        textarea.selectionStart = textarea.selectionEnd =
                          start + indent.length;
                      });
                    } else if (e.key === "Backspace" && start === end) {
                      // Check if we should remove a full indent
                      const beforeCursor = code.substring(0, start);
                      const lineStart = beforeCursor.lastIndexOf("\n") + 1;
                      const lineBeforeCursor =
                        beforeCursor.substring(lineStart);

                      // Check if line before cursor is only spaces and we have at least one indent worth
                      if (
                        /^ +$/.test(lineBeforeCursor) &&
                        lineBeforeCursor.length > 0
                      ) {
                        e.preventDefault();
                        // Remove up to 4 spaces (or remaining spaces if less than 4)
                        const spacesToRemove =
                          ((lineBeforeCursor.length - 1) % 4) + 1;
                        const newCode =
                          code.substring(0, start - spacesToRemove) +
                          code.substring(end);
                        setCode(newCode);

                        requestAnimationFrame(() => {
                          textarea.selectionStart = textarea.selectionEnd =
                            start - spacesToRemove;
                        });
                      }
                    }
                  }}
                  className="absolute inset-0 resize-none bg-transparent py-4 pl-14 pr-4 font-mono text-sm leading-relaxed text-transparent caret-[#F59E0B] outline-none placeholder:text-white/30 selection:bg-[#F59E0B]/30"
                  placeholder="Write your Kronos code here..."
                  spellCheck={false}
                  style={{
                    tabSize: 4,
                    caretColor: "#F59E0B",
                  }}
                />
              </div>
            </div>

            {/* Output Panel */}
            <div className="flex flex-col overflow-hidden rounded-xl border border-white/10 bg-[#0a0a0a]/80 backdrop-blur-sm">
              <div className="flex items-center justify-between border-b border-white/10 bg-white/5 px-4 py-2">
                <div className="flex items-center gap-2">
                  <Terminal className="h-4 w-4 text-white/40" />
                  <span className="text-sm text-white/40">Output</span>
                </div>
                {/* WASM Status Indicator */}
                <div className="flex items-center gap-2">
                  {wasmStatus === "loading" && (
                    <span className="flex items-center gap-1.5 text-xs text-white/40">
                      <Loader2 className="h-3 w-3 animate-spin" />
                      Loading runtime...
                    </span>
                  )}
                  {wasmStatus === "ready" && (
                    <span className="flex items-center gap-1.5 text-xs text-green-400/80">
                      <span className="h-1.5 w-1.5 rounded-full bg-green-400" />
                      Ready
                    </span>
                  )}
                  {wasmStatus === "unavailable" && (
                    <span className="flex items-center gap-1.5 text-xs text-amber-400/80">
                      <AlertCircle className="h-3 w-3" />
                      WASM unavailable
                    </span>
                  )}
                </div>
              </div>
              <div className="flex-1 overflow-auto p-4">
                {output ? (
                  <pre className="font-mono text-sm leading-relaxed text-white/80 whitespace-pre-wrap">
                    {output}
                  </pre>
                ) : (
                  <div className="flex h-full flex-col items-center justify-center text-center">
                    {wasmStatus === "loading" ? (
                      <>
                        <div className="mb-4 flex h-16 w-16 items-center justify-center rounded-2xl bg-white/5">
                          <Loader2 className="h-8 w-8 text-white/20 animate-spin" />
                        </div>
                        <p className="text-sm text-white/40">
                          Initializing Kronos runtime...
                        </p>
                      </>
                    ) : wasmStatus === "unavailable" ? (
                      <>
                        <div className="mb-4 flex h-16 w-16 items-center justify-center rounded-2xl bg-amber-500/10">
                          <AlertCircle className="h-8 w-8 text-amber-400/50" />
                        </div>
                        <p className="text-sm text-white/40">
                          WASM module not available
                        </p>
                        <p className="mt-2 text-xs text-white/30">
                          Build with:{" "}
                          <code className="text-[#F59E0B]">make wasm</code>
                        </p>
                      </>
                    ) : (
                      <>
                        <div className="mb-4 flex h-16 w-16 items-center justify-center rounded-2xl bg-white/5">
                          <Play className="h-8 w-8 text-white/20" />
                        </div>
                        <p className="text-sm text-white/40">
                          Click <span className="text-[#F59E0B]">Run</span> to
                          execute your code
                        </p>
                        <p className="mt-1 text-xs text-white/30">
                          or press{" "}
                          <kbd className="rounded bg-white/10 px-1.5 py-0.5 font-mono">
                            ⌘
                          </kbd>{" "}
                          +{" "}
                          <kbd className="rounded bg-white/10 px-1.5 py-0.5 font-mono">
                            Enter
                          </kbd>
                        </p>
                      </>
                    )}
                  </div>
                )}
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
