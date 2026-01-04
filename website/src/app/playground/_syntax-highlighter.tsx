import React from "react";

/**
 * Syntax highlighting function based on Kronos TextMate grammar.
 * Converts Kronos code into an array of React nodes with appropriate color classes.
 */
export function highlightKronos(code: string): React.ReactNode[] {
  const lines = code.split("\n");

  return lines.map((line, lineIndex) => {
    const tokens: React.ReactNode[] = [];
    let remaining = line;
    let position = 0;

    while (remaining.length > 0) {
      let matched = false;

      // Comments - highest priority
      const commentMatch = remaining.match(/^(#.*)$/);
      if (commentMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#6B7280]">
            {commentMatch[1]}
          </span>
        );
        break;
      }

      // F-strings
      const fstringMatch = remaining.match(/^(f"(?:[^"\\]|\\.)*")/);
      if (fstringMatch) {
        const fstring = fstringMatch[1];
        // Highlight f-string with interpolations
        const parts: React.ReactNode[] = [];
        parts.push(
          <span key="f" className="text-[#F59E0B]">
            f
          </span>
        );
        let inString = fstring.substring(1);
        let sPos = 0;
        while (sPos < inString.length) {
          const braceMatch = inString.substring(sPos).match(/^(\{[^}]*\})/);
          if (braceMatch) {
            if (sPos > 0 || inString[0] === '"') {
              const beforeBrace = inString.substring(0, sPos);
              if (beforeBrace) {
                parts.push(
                  <span key={`str-${sPos}`} className="text-[#10B981]">
                    {beforeBrace}
                  </span>
                );
              }
            }
            parts.push(
              <span key={`brace-${sPos}`} className="text-[#F59E0B]">
                {braceMatch[1]}
              </span>
            );
            inString = inString.substring(sPos + braceMatch[1].length);
            sPos = 0;
          } else {
            sPos++;
          }
        }
        if (inString) {
          parts.push(
            <span key="rest" className="text-[#10B981]">
              {inString}
            </span>
          );
        }
        tokens.push(<span key={`${lineIndex}-${position}`}>{parts}</span>);
        remaining = remaining.substring(fstring.length);
        position += fstring.length;
        matched = true;
        continue;
      }

      // Regular strings
      const stringMatch = remaining.match(/^("[^"\\]*(?:\\.[^"\\]*)*")/);
      if (stringMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#10B981]">
            {stringMatch[1]}
          </span>
        );
        remaining = remaining.substring(stringMatch[1].length);
        position += stringMatch[1].length;
        matched = true;
        continue;
      }

      // Control keywords
      const controlMatch = remaining.match(
        /^(if|else|for|while|in|range|return|with|break|continue|try|catch|finally|raise|delete|function|call|import)\b/
      );
      if (controlMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#8B5CF6]">
            {controlMatch[1]}
          </span>
        );
        remaining = remaining.substring(controlMatch[1].length);
        position += controlMatch[1].length;
        matched = true;
        continue;
      }

      // Storage keywords
      const storageMatch = remaining.match(/^(let|set)\b/);
      if (storageMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#8B5CF6]">
            {storageMatch[1]}
          </span>
        );
        remaining = remaining.substring(storageMatch[1].length);
        position += storageMatch[1].length;
        matched = true;
        continue;
      }

      // Other keywords
      const otherKeywordMatch = remaining.match(
        /^(print|to|as|is|than|by|at)\b/
      );
      if (otherKeywordMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#8B5CF6]">
            {otherKeywordMatch[1]}
          </span>
        );
        remaining = remaining.substring(otherKeywordMatch[1].length);
        position += otherKeywordMatch[1].length;
        matched = true;
        continue;
      }

      // Operators
      const operatorMatch = remaining.match(
        /^(plus|minus|times|divided|mod|modulo|equal|not|greater|less|and|or)\b/
      );
      if (operatorMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#EC4899]">
            {operatorMatch[1]}
          </span>
        );
        remaining = remaining.substring(operatorMatch[1].length);
        position += operatorMatch[1].length;
        matched = true;
        continue;
      }

      // Built-in functions
      const builtinMatch = remaining.match(
        /^(add|subtract|multiply|divide|len|uppercase|lowercase|trim|split|join|contains|starts_with|ends_with|replace|to_string|to_number|to_bool|reverse|sort|sqrt|power|abs|round|floor|ceil|rand|min|max|list|map|read_file|write_file)\b/
      );
      if (builtinMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#F59E0B]">
            {builtinMatch[1]}
          </span>
        );
        remaining = remaining.substring(builtinMatch[1].length);
        position += builtinMatch[1].length;
        matched = true;
        continue;
      }

      // Constants
      const constantMatch = remaining.match(
        /^(true|false|null|undefined|Pi)\b/
      );
      if (constantMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#F59E0B]">
            {constantMatch[1]}
          </span>
        );
        remaining = remaining.substring(constantMatch[1].length);
        position += constantMatch[1].length;
        matched = true;
        continue;
      }

      // Numbers
      const numberMatch = remaining.match(/^(\d+(?:\.\d+)?)\b/);
      if (numberMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-[#A78BFA]">
            {numberMatch[1]}
          </span>
        );
        remaining = remaining.substring(numberMatch[1].length);
        position += numberMatch[1].length;
        matched = true;
        continue;
      }

      // Identifiers (variables, function names)
      const identifierMatch = remaining.match(/^([a-zA-Z_][a-zA-Z0-9_]*)/);
      if (identifierMatch) {
        tokens.push(
          <span key={`${lineIndex}-${position}`} className="text-white/90">
            {identifierMatch[1]}
          </span>
        );
        remaining = remaining.substring(identifierMatch[1].length);
        position += identifierMatch[1].length;
        matched = true;
        continue;
      }

      // Default: single character (whitespace, punctuation, etc.)
      if (!matched) {
        const char = remaining[0];
        if (char === ":" || char === ",") {
          tokens.push(
            <span key={`${lineIndex}-${position}`} className="text-[#6B7280]">
              {char}
            </span>
          );
        } else {
          tokens.push(
            <span key={`${lineIndex}-${position}`} className="text-white/90">
              {char}
            </span>
          );
        }
        remaining = remaining.substring(1);
        position += 1;
      }
    }

    // Return the line with a newline for all but the last line
    return (
      <div key={lineIndex} className="whitespace-pre">
        {tokens.length > 0 ? tokens : " "}
      </div>
    );
  });
}
