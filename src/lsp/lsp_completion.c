/**
 * @file lsp_completion.c
 * @brief Autocomplete logic for LSP server
 */

#include "lsp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

void handle_completion(const char *id, const char *body) {
  // TODO: Use body parameter to get position and context for context-aware completions
  // Currently returns all possible completions regardless of position
  // The body contains params.position.line/character for filtering suggestions
  (void)body; // Placeholder for context-aware completion implementation
  // Build completion list with keywords, built-ins, and symbols
  char completions[LSP_REFERENCES_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(completions);

  pos += snprintf(completions + pos, remaining - pos,
                  "{\"isIncomplete\":false,\"items\":[");

  bool first = true;

  // Add keywords
  const char *keywords[][2] = {
      // Variable declarations
      {"set", "Immutable variable"},
      {"let", "Mutable variable"},
      {"to", "Assignment operator (set x to 5)"},
      {"as", "Type annotation (as number)"},
      // Control flow
      {"if", "Conditional statement"},
      {"else", "Else clause"},
      {"else if", "Else-if clause"},
      {"for", "For loop"},
      {"in", "Loop iterator (for x in list)"},
      {"while", "While loop"},
      {"break", "Break out of loop"},
      {"continue", "Continue to next iteration"},
      {"delete", "Delete map key (delete var at key)"},
      // Exception handling
      {"try", "Try block (exception handling)"},
      {"catch", "Catch exception (catch ErrorType as var)"},
      {"finally", "Finally block (always executes)"},
      {"raise", "Raise exception (raise ErrorType \"message\")"},
      // Logical operators
      {"and", "Logical AND operator"},
      {"or", "Logical OR operator"},
      {"not", "Logical NOT operator"},
      // Arithmetic operators
      {"plus", "Addition operator"},
      {"minus", "Subtraction operator"},
      {"times", "Multiplication operator"},
      {"divided", "Division operator"},
      {"by", "Step value or division (divided by)"},
      {"mod", "Modulo operator"},
      // Comparison operators
      {"is", "Comparison prefix (is equal to)"},
      {"equal", "Equality comparison"},
      {"greater", "Greater than comparison"},
      {"less", "Less than comparison"},
      {"than", "Comparison suffix (greater than)"},
      // Data structures
      {"list", "Create list literal"},
      {"map", "Create map literal"},
      {"range", "Create range literal (range 1 to 10)"},
      {"at", "List/map indexing operator"},
      {"from", "List slicing operator"},
      {"end", "End of list (for slicing)"},
      // Functions
      {"function", "Define function"},
      {"call", "Call function"},
      {"with", "Function arguments (call fn with args)"},
      {"return", "Return value"},
      // Modules
      {"import", "Import module"},
      // I/O
      {"print", "Print value"},
      // Literals
      {"true", "Boolean true"},
      {"false", "Boolean false"},
      {"null", "Null value"},
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if (!first) {
      int written = snprintf(completions + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;
    char escaped[LSP_PATTERN_BUFFER_SIZE];
    json_escape(keywords[i][0], escaped, sizeof(escaped));
    char escaped_detail[LSP_PATTERN_BUFFER_SIZE];
    json_escape(keywords[i][1], escaped_detail, sizeof(escaped_detail));
    int written = snprintf(completions + pos, remaining - pos,
                           "{\"label\":\"%s\",\"kind\":14,\"detail\":\"%s\"}",
                           escaped, escaped_detail);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }
  }

  // Add built-in functions
  const char *builtins[][2] = {
      {"len", "Get length of list, string, or range"},
      {"uppercase", "Convert string to uppercase"},
      {"lowercase", "Convert string to lowercase"},
      {"trim", "Remove leading and trailing whitespace"},
      {"split", "Split string by delimiter into list"},
      {"join", "Join list of strings with delimiter"},
      {"to_string", "Convert value to string"},
      {"to_number", "Convert string to number"},
      {"to_bool", "Convert value to boolean"},
      {"contains", "Check if string contains substring"},
      {"starts_with", "Check if string starts with prefix"},
      {"ends_with", "Check if string ends with suffix"},
      {"replace", "Replace all occurrences (string, old, new)"},
      {"sqrt", "Square root of a number"},
      {"power", "Raise base to exponent"},
      {"abs", "Absolute value of a number"},
      {"round", "Round number to nearest integer"},
      {"floor", "Floor of a number"},
      {"ceil", "Ceiling of a number"},
      {"rand", "Random number between 0 and 1 (no args)"},
      {"min", "Minimum of numbers"},
      {"max", "Maximum of numbers"},
      {"reverse", "Reverse a list"},
      {"sort", "Sort a list"},
      {"read_file", "Read entire file content as string"},
      {"write_file", "Write string content to file (path, content)"},
      {"read_lines", "Read file and return list of lines"},
      {"file_exists", "Check if file or directory exists"},
      {"list_files", "List files in directory"},
      {"join_path", "Join two path components (path1, path2)"},
      {"dirname", "Get directory name from path"},
      {"basename", "Get file name from path"},
      {"regex.match",
       "Check if pattern matches entire string (string, pattern)"},
      {"regex.search", "Find first match in string (string, pattern)"},
      {"regex.findall", "Find all matches in string (string, pattern)"},
  };

  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
    if (!first) {
      int written = snprintf(completions + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;
    char escaped[LSP_PATTERN_BUFFER_SIZE];
    json_escape(builtins[i][0], escaped, sizeof(escaped));
    char escaped_detail[LSP_PATTERN_BUFFER_SIZE];
    json_escape(builtins[i][1], escaped_detail, sizeof(escaped_detail));
    int written = snprintf(completions + pos, remaining - pos,
                           "{\"label\":\"%s\",\"kind\":3,\"detail\":\"%s\"}",
                           escaped, escaped_detail);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }
  }

  // Add document symbols (variables and functions)
  if (g_doc && g_doc->symbols) {
    Symbol *sym = g_doc->symbols;
    while (sym) {
      size_t available = remaining - pos;
      if (available == 0) {
        break; // No space left
      }

      // Add comma if needed
      if (!first) {
        int n = snprintf(completions + pos, available, ",");
        if (n < 0 || (size_t)n >= available) {
          break; // Error or buffer full
        }
        pos += (size_t)n;
        remaining -= (size_t)n;
        available = remaining - pos;
        if (available == 0) {
          break; // No space left after comma
        }
      }
      first = false;

      const char *kind_str = "6"; // Variable
      if (sym->type == SYMBOL_FUNCTION)
        kind_str = "12"; // Function

      char escaped[LSP_PATTERN_BUFFER_SIZE];
      json_escape(sym->name, escaped, sizeof(escaped));
      const char *detail =
          sym->type == SYMBOL_FUNCTION ? "User-defined function" : "Variable";
      char escaped_detail[LSP_PATTERN_BUFFER_SIZE];
      json_escape(detail, escaped_detail, sizeof(escaped_detail));

      int n = snprintf(completions + pos, available,
                       "{\"label\":\"%s\",\"kind\":%s,\"detail\":\"%s\"}",
                       escaped, kind_str, escaped_detail);
      if (n < 0 || (size_t)n >= available) {
        break; // Error or buffer full
      }
      pos += (size_t)n;
      remaining -= (size_t)n;
      sym = sym->next;
    }
  }

  // Add constants
  if (!first)
    pos += snprintf(completions + pos, remaining - pos, ",");
  pos += snprintf(completions + pos, remaining - pos,
                  "{\"label\":\"Pi\",\"kind\":21,\"detail\":\"Mathematical "
                  "constant\"}");

  pos += snprintf(completions + pos, remaining - pos, "]}");
  send_response(id, completions);
}
