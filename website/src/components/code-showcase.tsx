"use client";

import { useState } from "react";
import { cn } from "~/lib/utils";

const codeExamples = [
  {
    name: "Functions",
    lines: [
      {
        text: "function fibonacci with n:",
        tokens: [
          { type: "keyword", value: "function" },
          { type: "function", value: " fibonacci" },
          { type: "keyword", value: " with" },
          { type: "variable", value: " n" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: "    if n is less than 2:",
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "if" },
          { type: "variable", value: " n" },
          { type: "keyword", value: " is less than" },
          { type: "number", value: " 2" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: "        return n",
        tokens: [
          { type: "plain", value: "        " },
          { type: "keyword", value: "return" },
          { type: "variable", value: " n" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "    set a to 0",
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "set" },
          { type: "variable", value: " a" },
          { type: "keyword", value: " to" },
          { type: "number", value: " 0" },
        ],
      },
      {
        text: "    set b to 1",
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "set" },
          { type: "variable", value: " b" },
          { type: "keyword", value: " to" },
          { type: "number", value: " 1" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "    for i in range 2 to n:",
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "for" },
          { type: "variable", value: " i" },
          { type: "keyword", value: " in" },
          { type: "function", value: " range" },
          { type: "number", value: " 2" },
          { type: "keyword", value: " to" },
          { type: "variable", value: " n" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: "        set temp to b",
        tokens: [
          { type: "plain", value: "        " },
          { type: "keyword", value: "set" },
          { type: "variable", value: " temp" },
          { type: "keyword", value: " to" },
          { type: "variable", value: " b" },
        ],
      },
      {
        text: "        let b to a plus b",
        tokens: [
          { type: "plain", value: "        " },
          { type: "keyword", value: "let" },
          { type: "variable", value: " b" },
          { type: "keyword", value: " to" },
          { type: "variable", value: " a" },
          { type: "keyword", value: " plus" },
          { type: "variable", value: " b" },
        ],
      },
      {
        text: "        let a to temp",
        tokens: [
          { type: "plain", value: "        " },
          { type: "keyword", value: "let" },
          { type: "variable", value: " a" },
          { type: "keyword", value: " to" },
          { type: "variable", value: " temp" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "    return b",
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "return" },
          { type: "variable", value: " b" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "print call fibonacci with 10",
        tokens: [
          { type: "function", value: "print" },
          { type: "keyword", value: " call" },
          { type: "function", value: " fibonacci" },
          { type: "keyword", value: " with" },
          { type: "number", value: " 10" },
        ],
      },
    ],
  },
  {
    name: "Error Handling",
    lines: [
      {
        text: "try:",
        tokens: [
          { type: "keyword", value: "try" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: '    set content to call read_file with "config.txt"',
        tokens: [
          { type: "plain", value: "    " },
          { type: "keyword", value: "set" },
          { type: "variable", value: " content" },
          { type: "keyword", value: " to" },
          { type: "keyword", value: " call" },
          { type: "function", value: " read_file" },
          { type: "keyword", value: " with" },
          { type: "string", value: ' "config.txt"' },
        ],
      },
      {
        text: "    print content",
        tokens: [
          { type: "plain", value: "    " },
          { type: "function", value: "print" },
          { type: "variable", value: " content" },
        ],
      },
      {
        text: "catch error:",
        tokens: [
          { type: "keyword", value: "catch" },
          { type: "variable", value: " error" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: '    print f"Failed to read file: {error}"',
        tokens: [
          { type: "plain", value: "    " },
          { type: "function", value: "print" },
          { type: "string", value: ' f"Failed to read file: {error}"' },
        ],
      },
      {
        text: "finally:",
        tokens: [
          { type: "keyword", value: "finally" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: '    print "Cleanup complete"',
        tokens: [
          { type: "plain", value: "    " },
          { type: "function", value: "print" },
          { type: "string", value: ' "Cleanup complete"' },
        ],
      },
    ],
  },
  {
    name: "Collections",
    lines: [
      {
        text: 'set user to map name: "Alice", age: 30, active: true',
        tokens: [
          { type: "keyword", value: "set" },
          { type: "variable", value: " user" },
          { type: "keyword", value: " to" },
          { type: "function", value: " map" },
          { type: "variable", value: " name" },
          { type: "punctuation", value: ":" },
          { type: "string", value: ' "Alice"' },
          { type: "punctuation", value: "," },
          { type: "variable", value: " age" },
          { type: "punctuation", value: ":" },
          { type: "number", value: " 30" },
          { type: "punctuation", value: "," },
          { type: "variable", value: " active" },
          { type: "punctuation", value: ":" },
          { type: "keyword", value: " true" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: 'print user at "name"',
        tokens: [
          { type: "function", value: "print" },
          { type: "variable", value: " user" },
          { type: "keyword", value: " at" },
          { type: "string", value: ' "name"' },
        ],
      },
      {
        text: 'set age to user at "age"',
        tokens: [
          { type: "keyword", value: "set" },
          { type: "variable", value: " age" },
          { type: "keyword", value: " to" },
          { type: "variable", value: " user" },
          { type: "keyword", value: " at" },
          { type: "string", value: ' "age"' },
        ],
      },
      {
        text: 'delete user at "active"',
        tokens: [
          { type: "keyword", value: "delete" },
          { type: "variable", value: " user" },
          { type: "keyword", value: " at" },
          { type: "string", value: ' "active"' },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "set scores to list 95, 87, 92, 78",
        tokens: [
          { type: "keyword", value: "set" },
          { type: "variable", value: " scores" },
          { type: "keyword", value: " to" },
          { type: "function", value: " list" },
          { type: "number", value: " 95" },
          { type: "punctuation", value: "," },
          { type: "number", value: " 87" },
          { type: "punctuation", value: "," },
          { type: "number", value: " 92" },
          { type: "punctuation", value: "," },
          { type: "number", value: " 78" },
        ],
      },
      {
        text: "for score in scores:",
        tokens: [
          { type: "keyword", value: "for" },
          { type: "variable", value: " score" },
          { type: "keyword", value: " in" },
          { type: "variable", value: " scores" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: '    print f"Score: {score}"',
        tokens: [
          { type: "plain", value: "    " },
          { type: "function", value: "print" },
          { type: "string", value: ' f"Score: {score}"' },
        ],
      },
    ],
  },
  {
    name: "Modules",
    lines: [
      {
        text: "import regex",
        tokens: [
          { type: "keyword", value: "import" },
          { type: "variable", value: " regex" },
        ],
      },
      { text: "", tokens: [] },
      {
        text: 'set text to "The cat sat on the mat"',
        tokens: [
          { type: "keyword", value: "set" },
          { type: "variable", value: " text" },
          { type: "keyword", value: " to" },
          { type: "string", value: ' "The cat sat on the mat"' },
        ],
      },
      {
        text: 'set matches to call regex.findall with text, "[a-z]at"',
        tokens: [
          { type: "keyword", value: "set" },
          { type: "variable", value: " matches" },
          { type: "keyword", value: " to" },
          { type: "keyword", value: " call" },
          { type: "function", value: " regex.findall" },
          { type: "keyword", value: " with" },
          { type: "variable", value: " text" },
          { type: "punctuation", value: "," },
          { type: "string", value: ' "[a-z]at"' },
        ],
      },
      { text: "", tokens: [] },
      {
        text: "for match in matches:",
        tokens: [
          { type: "keyword", value: "for" },
          { type: "variable", value: " match" },
          { type: "keyword", value: " in" },
          { type: "variable", value: " matches" },
          { type: "punctuation", value: ":" },
        ],
      },
      {
        text: '    print f"Found: {match}"',
        tokens: [
          { type: "plain", value: "    " },
          { type: "function", value: "print" },
          { type: "string", value: ' f"Found: {match}"' },
        ],
      },
    ],
  },
];

const tokenStyles: Record<string, string> = {
  keyword: "text-[#8B5CF6]",
  function: "text-[#F59E0B]",
  variable: "text-white",
  string: "text-green-400",
  number: "text-[#A78BFA]",
  punctuation: "text-[#4B5563]",
  comment: "text-[#4B5563]",
  plain: "text-white",
};

export function CodeShowcase() {
  const [activeTab, setActiveTab] = useState(0);

  return (
    <section className="relative px-4 py-24 sm:px-6 lg:px-8">
      <div className="mx-auto max-w-4xl">
        <div className="mb-12 text-center">
          <h2 className="mb-4 text-3xl font-bold text-white sm:text-4xl">
            Expressive & Powerful
          </h2>
          <p className="mx-auto max-w-2xl text-[#9CA3AF]">
            Clean syntax that gets out of your way. Powerful features when you
            need them.
          </p>
        </div>

        <div className="relative">
          <div className="absolute -inset-4 rounded-3xl bg-[rgba(245,158,11,0.1)] blur-2xl" />

          <div className="relative overflow-hidden rounded-2xl border border-[rgba(245,158,11,0.15)] bg-[#0a0a0a]/80 backdrop-blur-xl">
            <div className="flex border-b border-white/10 bg-white/5">
              {codeExamples.map((example, index) => (
                <button
                  key={example.name}
                  onClick={() => setActiveTab(index)}
                  className={cn(
                    "relative px-4 py-3 text-sm font-medium transition-colors",
                    activeTab === index
                      ? "text-white"
                      : "text-[#4B5563] hover:text-[#9CA3AF]"
                  )}
                >
                  {example.name}
                  {activeTab === index && (
                    <div className="absolute bottom-0 left-0 right-0 h-0.5 bg-[#F59E0B]" />
                  )}
                </button>
              ))}
            </div>

            <div className="p-6">
              <pre className="overflow-x-auto">
                <code className="font-mono text-sm leading-relaxed">
                  {codeExamples[activeTab].lines.map((line, i) => (
                    <div key={i} className="flex">
                      <span className="mr-4 inline-block w-8 select-none text-right text-[#4B5563]">
                        {i + 1}
                      </span>
                      <span>
                        {line.tokens.map((token, j) => (
                          <span key={j} className={tokenStyles[token.type]}>
                            {token.value}
                          </span>
                        ))}
                      </span>
                    </div>
                  ))}
                </code>
              </pre>
            </div>
          </div>
        </div>
      </div>
    </section>
  );
}
