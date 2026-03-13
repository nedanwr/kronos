/**
 * @file lsp_handlers.c
 * @brief Basic LSP request handlers
 */

#include "lsp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern DocumentState *g_doc;

typedef enum {
  FORMAT_BLOCK_NONE = 0,
  FORMAT_BLOCK_OTHER,
  FORMAT_BLOCK_MATCH,
  FORMAT_BLOCK_MATCH_BRANCH,
} FormatBlockType;

static bool starts_with_keyword(const char *text, const char *keyword) {
  size_t len = strlen(keyword);
  if (strncmp(text, keyword, len) != 0) {
    return false;
  }

  char next = text[len];
  return next == '\0' || isspace((unsigned char)next) || next == ':';
}

void handle_initialize(const char *id) {
  const char *capabilities =
      "{"
      "\"capabilities\":{"
      "\"textDocumentSync\":1,"
      "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
      "\"definitionProvider\":true,"
      "\"hoverProvider\":true,"
      "\"documentSymbolProvider\":true,"
      "\"signatureHelpProvider\":{\"triggerCharacters\":[\",\",\" \"],\"retriggerCharacters\":[\",\"]},"
      "\"inlayHintProvider\":true,"
      "\"callHierarchyProvider\":true,"
      "\"foldingRangeProvider\":true,"
      "\"semanticTokensProvider\":{"
      "\"legend\":{"
      "\"tokenTypes\":[\"variable\",\"function\",\"parameter\",\"keyword\",\"number\",\"string\",\"operator\"],"
      "\"tokenModifiers\":[\"unused\",\"readonly\"]"
      "},"
      "\"range\":false,"
      "\"full\":{\"delta\":false}"
      "},"
      "\"experimental\":{\"bracketPairColorization\":{\"enabled\":true}}"
      "}"
      "}";
  send_response(id, capabilities);
}

void handle_shutdown(const char *id) { send_response(id, "null"); }

void handle_did_open(const char *uri, const char *text) {
  // Update or create document state
  if (g_doc) {
    free_document_state(g_doc);
    g_doc = NULL;
  }

  // Allocate document state structure
  g_doc = malloc(sizeof(DocumentState));
  if (!g_doc) {
    fprintf(stderr, "LSP server: failed to allocate DocumentState\n");
    return;
  }

  // Allocate URI and text into temporary pointers first
  char *uri_copy = strdup(uri);
  if (!uri_copy) {
    fprintf(stderr, "LSP server: failed to allocate URI string\n");
    free(g_doc);
    g_doc = NULL;
    return;
  }

  char *text_copy = strdup(text);
  if (!text_copy) {
    fprintf(stderr, "LSP server: failed to allocate text string\n");
    free(uri_copy);
    free(g_doc);
    g_doc = NULL;
    return;
  }

  // All allocations succeeded - set fields
  g_doc->uri = uri_copy;
  g_doc->text = text_copy;
  g_doc->symbols = NULL;
  g_doc->ast = NULL;
  g_doc->imported_modules = NULL;

  check_diagnostics(uri, text, true);
}

void handle_did_change(const char *uri, const char *text) {
  // Update document text
  if (g_doc && g_doc->uri && strcmp(g_doc->uri, uri) == 0) {
    char *text_copy = strdup(text);
    if (!text_copy) {
      fprintf(stderr, "LSP server: failed to allocate text string for did_change\n");
      // Use existing g_doc->text as fallback for diagnostics
      check_diagnostics(uri, g_doc->text, false);
      return;
    }
    free(g_doc->text);
    g_doc->text = text_copy;
    check_diagnostics(uri, text, false);
  }
}

void handle_code_action(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "[]");
    return;
  }

  // Parse position from body
  char *line_str = json_get_nested_value(body, "params.range.start.line");
  char *character_str = json_get_nested_value(body, "params.range.start.character");

  if (!line_str || !character_str) {
    free(line_str);
    free(character_str);
    send_response(id, "[]");
    return;
  }

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "[]");
    return;
  }
  free(line_str);
  free(character_str);

  // Basic implementation: return empty array for now
  // Future enhancements could include:
  // - Quick fixes for undefined variables (suggest imports or declarations)
  // - Type error fixes (suggest type conversions)
  // - Unused variable removal
  // - Import organization
  send_response(id, "[]");
}

void handle_formatting(const char *id,
                             const char *body __attribute__((unused))) {
  // TODO: Use body parameter to get formatting options (tab size, insert spaces, etc.)
  // Currently uses global document text and default formatting rules
  (void)body; // Placeholder for future implementation
  if (!g_doc || !g_doc->text) {
    send_response(id, "null");
    return;
  }

  // Basic formatting: ensure consistent indentation (4 spaces)
  // and proper spacing around operators
  const char *text = g_doc->text;
  size_t text_len = strlen(text);
  size_t formatted_capacity = text_len * 2 + 1;
  char *formatted = malloc(formatted_capacity); // Allocate extra space
  if (!formatted) {
    send_response(id, "null");
    return; // No memory allocated, nothing to free
  }
  // VERIFICATION: formatted is ALWAYS freed at line 189 before function returns
  // Execution path analysis:
  // 1. Line 84-87: Early return if !g_doc - BEFORE malloc, no leak
  // 2. Line 93: malloc(text_len * 2 + 1) - ALLOCATION
  // 3. Line 94-97: Early return if !formatted - malloc failed, nothing to free, no leak
  // 4. Line 106-167: Loop - only has 'continue', no 'return' statements
  // 5. Line 174: json_escape() - void function, always completes, no early return
  // 6. Line 184-187: snprintf() - may truncate but continues, no early return
  // 7. Line 189: free(formatted) - ALWAYS EXECUTED (verified by code analysis and test)
  // Conclusion: No memory leak exists - this is a false positive

  size_t out_pos = 0;
  int indent_level = 0;
  bool at_line_start = true;
  bool last_was_space = false;
  FormatBlockType current_line_block = FORMAT_BLOCK_NONE;
  FormatBlockType block_stack[256];
  size_t block_depth = 0;

  for (size_t i = 0; i < text_len && out_pos < formatted_capacity - 1; i++) {
    char c = text[i];

    if (c == '\n') {
      formatted[out_pos++] = '\n';
      at_line_start = true;
      last_was_space = false;
      current_line_block = FORMAT_BLOCK_NONE;
      continue;
    }

    if (at_line_start) {
      // Skip leading whitespace
      if (isspace((unsigned char)c) && c != '\n') {
        continue;
      }

      bool is_case_or_default = starts_with_keyword(text + i, "case") ||
                                starts_with_keyword(text + i, "default");
      bool is_else_like = starts_with_keyword(text + i, "else") ||
                          starts_with_keyword(text + i, "catch") ||
                          starts_with_keyword(text + i, "finally");

      if (is_case_or_default) {
        while (block_depth > 0 &&
               block_stack[block_depth - 1] != FORMAT_BLOCK_MATCH) {
          block_depth--;
          if (indent_level > 0) {
            indent_level--;
          }
        }
      } else if (is_else_like) {
        if (indent_level > 0) {
          indent_level--;
        }
        if (block_depth > 0) {
          block_depth--;
        }
      }

      if (starts_with_keyword(text + i, "match")) {
        current_line_block = FORMAT_BLOCK_MATCH;
      } else if (is_case_or_default) {
        current_line_block = FORMAT_BLOCK_MATCH_BRANCH;
      } else {
        current_line_block = FORMAT_BLOCK_OTHER;
      }

      // Apply indentation
      int spaces = indent_level * 4;
      for (int j = 0; j < spaces && out_pos < formatted_capacity - 1; j++) {
        formatted[out_pos++] = ' ';
      }
      at_line_start = false;
    }

    // Handle indentation changes
    if (c == ':' && i + 1 < text_len && text[i + 1] == '\n') {
      formatted[out_pos++] = c;
      indent_level++;
      if (block_depth < sizeof(block_stack) / sizeof(block_stack[0])) {
        block_stack[block_depth++] = current_line_block == FORMAT_BLOCK_NONE
                                         ? FORMAT_BLOCK_OTHER
                                         : current_line_block;
      }
      last_was_space = false;
      continue;
    }

    // Normalize whitespace
    if (isspace((unsigned char)c)) {
      if (!last_was_space && c == ' ') {
        formatted[out_pos++] = ' ';
        last_was_space = true;
      }
      continue;
    }

    last_was_space = false;
    formatted[out_pos++] = c;

    // Check for dedent keywords
    if (strncmp(text + i, "return", 6) == 0 ||
        strncmp(text + i, "break", 5) == 0 ||
        strncmp(text + i, "continue", 8) == 0) {
      // These don't change indent, but might be followed by dedent
    }
  }

  formatted[out_pos] = '\0';

  // Create TextEdit for the entire document
  char result[LSP_LARGE_BUFFER_SIZE];
  char escaped_text[LSP_REFERENCES_BUFFER_SIZE];
  json_escape(formatted, escaped_text, sizeof(escaped_text));

  // Calculate actual line count
  size_t line_count = 1;
  for (size_t i = 0; i < out_pos; i++) {
    if (formatted[i] == '\n') {
      line_count++;
    }
  }

  snprintf(result, sizeof(result),
           "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
           "\"end\":{\"line\":%zu,\"character\":0}},\"newText\":\"%s\"}]",
           line_count > 0 ? line_count - 1 : 0, escaped_text);

  free(formatted);
  send_response(id, result);
}

void handle_document_symbols(const char *id) {
  if (!g_doc || !g_doc->symbols) {
    send_response(id, "[]");
    return;
  }

  char symbols[LSP_INITIAL_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(symbols);
  bool first = true;

  pos += snprintf(symbols + pos, remaining - pos, "[");

  Symbol *sym = g_doc->symbols;
  while (sym && pos < remaining - 200) {
    if (!first)
      pos += snprintf(symbols + pos, remaining - pos, ",");
    first = false;

    const char *kind_str = "6"; // Variable
    if (sym->type == SYMBOL_FUNCTION)
      kind_str = "12"; // Function
    else if (sym->type == SYMBOL_PARAMETER)
      kind_str = "5"; // Property
    else if (sym->type == SYMBOL_TYPE_ALIAS)
      kind_str = "13"; // Type parameter-like

    char *escaped_name = malloc(strlen(sym->name) * 2 + 1);
    if (escaped_name) {
      json_escape(sym->name, escaped_name, strlen(sym->name) * 2 + 1);
      pos += snprintf(symbols + pos, remaining - pos,
                      "{\"name\":\"%s\",\"kind\":%s,"
                      "\"location\":{\"uri\":\"%s\","
                      "\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                      "\"end\":{\"line\":%zu,\"character\":%zu}}}}",
                      escaped_name, kind_str, g_doc->uri, sym->line - 1,
                      sym->column - 1, sym->line - 1,
                      sym->column - 1 + strlen(sym->name));
      free(escaped_name);
    }
    sym = sym->next;
  }

  pos += snprintf(symbols + pos, remaining - pos, "]");
  send_response(id, symbols);
}

void handle_workspace_symbol(const char *id, const char *body) {
  if (!g_doc || !g_doc->symbols) {
    send_response(id, "[]");
    return;
  }

  // Get query string from request
  char *query = json_get_nested_value(body, "params.query");
  if (!query || strlen(query) == 0) {
    // Empty query - return all symbols
    handle_document_symbols(id);
    free(query);
    return;
  }

  // Convert query to lowercase for case-insensitive matching
  char query_lower[LSP_PATTERN_BUFFER_SIZE];
  size_t query_len = strlen(query);
  if (query_len >= sizeof(query_lower)) {
    query_len = sizeof(query_lower) - 1;
  }
  for (size_t i = 0; i < query_len; i++) {
    query_lower[i] = (char)tolower((unsigned char)query[i]);
  }
  query_lower[query_len] = '\0';

  char symbols[LSP_INITIAL_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(symbols);
  bool first = true;

  pos += snprintf(symbols + pos, remaining - pos, "[");

  Symbol *sym = g_doc->symbols;
  while (sym && pos < remaining - 200) {
    // Case-insensitive partial match
    char name_lower[LSP_PATTERN_BUFFER_SIZE];
    size_t name_len = strlen(sym->name);
    if (name_len >= sizeof(name_lower)) {
      name_len = sizeof(name_lower) - 1;
    }
    for (size_t i = 0; i < name_len; i++) {
      name_lower[i] = (char)tolower((unsigned char)sym->name[i]);
    }
    name_lower[name_len] = '\0';

    // Check if query matches symbol name
    if (strstr(name_lower, query_lower) != NULL) {
      if (!first)
        pos += snprintf(symbols + pos, remaining - pos, ",");
      first = false;

      const char *kind_str = "6"; // Variable
      if (sym->type == SYMBOL_FUNCTION)
        kind_str = "12"; // Function
      else if (sym->type == SYMBOL_PARAMETER)
        kind_str = "5"; // Property
      else if (sym->type == SYMBOL_TYPE_ALIAS)
        kind_str = "13"; // Type parameter-like

      char *escaped_name = malloc(strlen(sym->name) * 2 + 1);
      if (escaped_name) {
        json_escape(sym->name, escaped_name, strlen(sym->name) * 2 + 1);
        char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
        json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));

        pos += snprintf(symbols + pos, remaining - pos,
                        "{\"name\":\"%s\",\"kind\":%s,"
                        "\"location\":{\"uri\":\"%s\","
                        "\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                        "\"end\":{\"line\":%zu,\"character\":%zu}}}}",
                        escaped_name, kind_str, escaped_uri, sym->line - 1,
                        sym->column - 1, sym->line - 1,
                        sym->column - 1 + strlen(sym->name));
        free(escaped_name);
      }
    }
    sym = sym->next;
  }

  pos += snprintf(symbols + pos, remaining - pos, "]");
  free(query);
  send_response(id, symbols);
}

void handle_code_lens(const char *id,
                            const char *body __attribute__((unused))) {
  // TODO: Use body parameter to get document URI and range for code lens
  // Currently returns code lens for entire document
  (void)body; // Placeholder for future implementation
  if (!g_doc || !g_doc->symbols || !g_doc->ast) {
    send_response(id, "[]");
    return;
  }

  char lenses[LSP_INITIAL_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(lenses);
  bool first = true;

  pos += snprintf(lenses + pos, remaining - pos, "[");

  Symbol *sym = g_doc->symbols;
  while (sym && pos < remaining - 200) {
    // Only show code lens for functions and top-level variables
    if (sym->type == SYMBOL_FUNCTION ||
        (sym->type == SYMBOL_VARIABLE && sym->line > 0)) {
      // Count references
      size_t ref_count = count_symbol_references_for_symbol(sym, g_doc->ast);

      if (!first)
        pos += snprintf(lenses + pos, remaining - pos, ",");
      first = false;

      // Build code lens text
      char lens_text[LSP_STACK_PATTERN_SIZE];
      if (sym->type == SYMBOL_FUNCTION) {
        snprintf(lens_text, sizeof(lens_text), "%zu reference%s", ref_count,
                 ref_count == 1 ? "" : "s");
        if (sym->param_count > 0) {
          char temp[LSP_STACK_PATTERN_SIZE];
          snprintf(temp, sizeof(temp), "%s • %zu parameter%s", lens_text,
                   sym->param_count, sym->param_count == 1 ? "" : "s");
          strncpy(lens_text, temp, sizeof(lens_text) - 1);
          lens_text[sizeof(lens_text) - 1] = '\0';
        }
      } else {
        snprintf(lens_text, sizeof(lens_text), "%zu reference%s", ref_count,
                 ref_count == 1 ? "" : "s");
      }

      char escaped_text[LSP_PATTERN_BUFFER_SIZE];
      json_escape(lens_text, escaped_text, sizeof(escaped_text));

      pos += snprintf(lenses + pos, remaining - pos,
                      "{\"range\":{\"start\":{\"line\":%zu,\"character\":0},"
                      "\"end\":{\"line\":%zu,\"character\":0}},\"command\":{"
                      "\"title\":\"%s\","
                      "\"command\":\"\",\"arguments\":[]}}",
                      sym->line - 1, sym->line - 1, escaped_text);
    }
    sym = sym->next;
  }

  pos += snprintf(lenses + pos, remaining - pos, "]");
  send_response(id, lenses);
}

typedef enum {
  SEMANTIC_TOKEN_VARIABLE = 0,
  SEMANTIC_TOKEN_FUNCTION = 1,
  SEMANTIC_TOKEN_PARAMETER = 2,
  SEMANTIC_TOKEN_KEYWORD = 3,
  SEMANTIC_TOKEN_NUMBER = 4,
  SEMANTIC_TOKEN_STRING = 5,
  SEMANTIC_TOKEN_OPERATOR = 6,
} SemanticTokenType;

typedef struct {
  int line;
  int col;
  int length;
  int type;
  int modifiers;
} SemanticTokenEntry;

typedef struct {
  char caller[128];
  char callee[128];
  size_t line;
  size_t col;
  size_t length;
} CallEdge;

typedef struct {
  char name[128];
  size_t indent;
} FunctionContext;

typedef struct {
  const char *name;
  const char *documentation;
  const char *params[4];
  size_t param_count;
  size_t required_count;
  bool variadic;
} BuiltinSignature;

typedef struct {
  size_t start;
  size_t end;
  bool is_named;
} ArgSegment;

static const BuiltinSignature BUILTIN_SIGNATURES[] = {
    {"len", "Get length of list, string, or range", {"value"}, 1, 1, false},
    {"split", "Split string by delimiter into list", {"text", "delimiter"}, 2,
     2, false},
    {"replace", "Replace all occurrences in a string",
     {"text", "old", "new"}, 3, 3, false},
    {"filter", "Filter a list with a callback function", {"list", "callback"},
     2, 2, false},
    {"map", "Transform a list with a callback function", {"list", "callback"},
     2, 2, false},
    {"join", "Join list of strings with delimiter", {"list", "delimiter"}, 2,
     1, true},
    {"min", "Minimum value across numbers", {"values"}, 1, 1, true},
    {"max", "Maximum value across numbers", {"values"}, 1, 1, true},
    {"write_file", "Write string content to file (path, content)",
     {"path", "content"}, 2, 2, false},
    {"read_file", "Read entire file content as string", {"path"}, 1, 1, false},
    {"math.sqrt", "Square root of a number", {"number"}, 1, 1, false},
    {"regex.match", "Check if pattern matches entire string",
     {"text", "pattern"}, 2, 2, false},
    {"regex.search", "Find first regex match in string", {"text", "pattern"},
     2, 2, false},
    {"regex.findall", "Find all regex matches in string", {"text", "pattern"},
     2, 2, false},
};

static const char *const SEMANTIC_KEYWORDS[] = {
    "set",      "let",    "type",    "to",     "as",       "if",    "else",
    "for",      "in",     "while",   "break",  "continue", "delete", "try",
    "catch",    "finally","raise",   "and",    "or",       "not",   "plus",
    "minus",    "times",  "divided", "by",     "mod",      "is",    "equal",
    "greater",  "less",   "than",    "list",   "map",      "range", "at",
    "from",     "end",    "function","call",   "with",     "return","import",
    "print",    "debug",  "match",   "case",   "default",  "true",  "false",
    "null",
};

static bool append_jsonf(char **buffer, size_t *capacity, size_t *len,
                         const char *fmt, ...) {
  if (!buffer || !capacity || !len || !fmt) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return false;
  }

  size_t required = *len + (size_t)needed + 1;
  if (required > *capacity) {
    size_t new_capacity = *capacity == 0 ? 256 : *capacity;
    while (new_capacity < required) {
      new_capacity *= 2;
    }

    char *grown = realloc(*buffer, new_capacity);
    if (!grown) {
      return false;
    }

    *buffer = grown;
    *capacity = new_capacity;
  }

  va_start(args, fmt);
  vsnprintf(*buffer + *len, *capacity - *len, fmt, args);
  va_end(args);
  *len += (size_t)needed;
  return true;
}

static bool is_identifier_start_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_identifier_char(char c) {
  return is_identifier_start_char(c) || (c >= '0' && c <= '9');
}

static bool is_identifier_or_dot_char(char c) {
  return is_identifier_char(c) || c == '.';
}

static const char *strip_module_prefix(const char *name) {
  if (!name) {
    return NULL;
  }
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}

static const BuiltinSignature *find_builtin_signature(const char *name) {
  if (!name) {
    return NULL;
  }

  for (size_t i = 0; i < sizeof(BUILTIN_SIGNATURES) / sizeof(BUILTIN_SIGNATURES[0]);
       i++) {
    if (strcmp(BUILTIN_SIGNATURES[i].name, name) == 0) {
      return &BUILTIN_SIGNATURES[i];
    }
    const char *stripped = strip_module_prefix(BUILTIN_SIGNATURES[i].name);
    if (stripped && strcmp(stripped, name) == 0) {
      return &BUILTIN_SIGNATURES[i];
    }
  }

  return NULL;
}

static Symbol *find_function_symbol(const char *name) {
  if (!g_doc || !g_doc->symbols || !name) {
    return NULL;
  }

  for (Symbol *sym = g_doc->symbols; sym; sym = sym->next) {
    if (sym->type == SYMBOL_FUNCTION && sym->name && strcmp(sym->name, name) == 0) {
      return sym;
    }
  }

  return NULL;
}

static bool get_line_span(const char *text, size_t zero_based_line,
                          const char **out_start, size_t *out_len) {
  if (!text || !out_start || !out_len) {
    return false;
  }

  size_t current = 0;
  const char *start = text;
  const char *cursor = text;

  while (*cursor && current < zero_based_line) {
    if (*cursor == '\n') {
      current++;
      start = cursor + 1;
    }
    cursor++;
  }

  if (current != zero_based_line) {
    return false;
  }

  const char *end = start;
  while (*end && *end != '\n') {
    end++;
  }

  *out_start = start;
  *out_len = (size_t)(end - start);
  return true;
}

static size_t leading_indent_width(const char *line, size_t line_len) {
  size_t indent = 0;
  while (indent < line_len && (line[indent] == ' ' || line[indent] == '\t')) {
    indent++;
  }
  return indent;
}

static bool keyword_equals(const char *word, size_t len, const char *kw) {
  size_t kw_len = strlen(kw);
  return len == kw_len && strncmp(word, kw, len) == 0;
}

static bool is_semantic_keyword(const char *word, size_t len) {
  for (size_t i = 0; i < sizeof(SEMANTIC_KEYWORDS) / sizeof(SEMANTIC_KEYWORDS[0]);
       i++) {
    if (keyword_equals(word, len, SEMANTIC_KEYWORDS[i])) {
      return true;
    }
  }
  return false;
}

static bool push_semantic_token(SemanticTokenEntry **tokens, size_t *count,
                                size_t *capacity, int line, int col, int length,
                                int type, int modifiers) {
  if (!tokens || !count || !capacity || line < 0 || col < 0 || length <= 0) {
    return false;
  }

  if (*count >= *capacity) {
    size_t new_capacity = *capacity == 0 ? 64 : *capacity * 2;
    SemanticTokenEntry *grown = realloc(*tokens, new_capacity * sizeof(**tokens));
    if (!grown) {
      return false;
    }
    *tokens = grown;
    *capacity = new_capacity;
  }

  (*tokens)[*count].line = line;
  (*tokens)[*count].col = col;
  (*tokens)[*count].length = length;
  (*tokens)[*count].type = type;
  (*tokens)[*count].modifiers = modifiers;
  (*count)++;
  return true;
}

static int semantic_token_cmp(const void *a, const void *b) {
  const SemanticTokenEntry *left = (const SemanticTokenEntry *)a;
  const SemanticTokenEntry *right = (const SemanticTokenEntry *)b;

  if (left->line != right->line) {
    return left->line - right->line;
  }
  if (left->col != right->col) {
    return left->col - right->col;
  }
  if (left->type != right->type) {
    return left->type - right->type;
  }
  return left->length - right->length;
}

static bool add_symbol_semantic_tokens(SemanticTokenEntry **tokens, size_t *count,
                                       size_t *capacity) {
  if (!g_doc || !g_doc->text || !g_doc->symbols) {
    return true;
  }

  for (Symbol *sym = g_doc->symbols; sym; sym = sym->next) {
    if (!sym->name || sym->type == SYMBOL_PARAMETER) {
      continue;
    }

    size_t line = 0;
    size_t col = 0;

    if (sym->line > 0) {
      line = sym->line - 1;
    }

    if (sym->type == SYMBOL_FUNCTION) {
      char pattern[LSP_PATTERN_BUFFER_SIZE];
      size_t found_line = 1;
      size_t found_col = 0;
      snprintf(pattern, sizeof(pattern), "function %s with", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &found_line, &found_col);
      if (found_line == 1 && found_col == 0) {
        snprintf(pattern, sizeof(pattern), "function %s", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &found_line, &found_col);
      }
      if (!(found_line == 1 && found_col == 0)) {
        line = found_line - 1;
        col = found_col + 9; // Skip "function "
      }
    } else {
      size_t found_line = 1;
      size_t found_col = 0;
      char pattern[LSP_PATTERN_BUFFER_SIZE];
      snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &found_line, &found_col);
      if (found_line == 1 && found_col == 0) {
        snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &found_line, &found_col);
      }
      if (!(found_line == 1 && found_col == 0)) {
        line = found_line - 1;
        col = found_col + 4; // Skip "set " / "let "
      }
    }

    int modifiers = 0;
    int token_type = sym->type == SYMBOL_FUNCTION ? SEMANTIC_TOKEN_FUNCTION
                                                  : SEMANTIC_TOKEN_VARIABLE;

    if (sym->type == SYMBOL_VARIABLE) {
      if (!sym->read) {
        modifiers |= 1; // unused
      }
      if (!sym->is_mutable) {
        modifiers |= 2; // readonly
      }
    } else if (sym->type == SYMBOL_FUNCTION && !sym->read && !sym->written) {
      modifiers |= 1; // unused
    }

    if (!push_semantic_token(tokens, count, capacity, (int)line, (int)col,
                             (int)strlen(sym->name), token_type, modifiers)) {
      return false;
    }
  }

  return true;
}

static bool add_lexical_semantic_tokens(SemanticTokenEntry **tokens, size_t *count,
                                        size_t *capacity) {
  if (!g_doc || !g_doc->text) {
    return true;
  }

  const char *text = g_doc->text;
  size_t i = 0;
  int line = 0;
  int col = 0;

  while (text[i] != '\0') {
    char c = text[i];

    if (c == '\n') {
      i++;
      line++;
      col = 0;
      continue;
    }

    if (c == '#') {
      while (text[i] != '\0' && text[i] != '\n') {
        i++;
        col++;
      }
      continue;
    }

    if (c == '"' || c == '\'') {
      char quote = c;
      int start_col = col;
      size_t start = i;
      i++;
      col++;
      while (text[i] != '\0' && text[i] != '\n') {
        if (text[i] == quote && text[i - 1] != '\\') {
          i++;
          col++;
          break;
        }
        i++;
        col++;
      }
      if (!push_semantic_token(tokens, count, capacity, line, start_col,
                               (int)(i - start), SEMANTIC_TOKEN_STRING, 0)) {
        return false;
      }
      continue;
    }

    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
      if (!push_semantic_token(tokens, count, capacity, line, col, 1,
                               SEMANTIC_TOKEN_OPERATOR, 0)) {
        return false;
      }
      i++;
      col++;
      continue;
    }

    if ((c >= '0' && c <= '9') ||
        ((c == '-' || c == '+') && (text[i + 1] >= '0' && text[i + 1] <= '9'))) {
      int start_col = col;
      size_t start = i;
      if (c == '-' || c == '+') {
        i++;
        col++;
      }
      while ((text[i] >= '0' && text[i] <= '9') || text[i] == '.') {
        i++;
        col++;
      }
      if (!push_semantic_token(tokens, count, capacity, line, start_col,
                               (int)(i - start), SEMANTIC_TOKEN_NUMBER, 0)) {
        return false;
      }
      continue;
    }

    if (is_identifier_start_char(c)) {
      int start_col = col;
      size_t start = i;
      while (is_identifier_char(text[i])) {
        i++;
        col++;
      }
      size_t len = i - start;
      if (is_semantic_keyword(text + start, len)) {
        if (!push_semantic_token(tokens, count, capacity, line, start_col,
                                 (int)len, SEMANTIC_TOKEN_KEYWORD, 0)) {
          return false;
        }
      }
      continue;
    }

    i++;
    col++;
  }

  return true;
}

static bool parse_call_context(const char *line, size_t line_len, size_t cursor_col,
                               char *func_name, size_t func_name_size,
                               size_t *active_param) {
  if (!line || !func_name || func_name_size == 0 || !active_param) {
    return false;
  }

  size_t best_call_start = SIZE_MAX;
  size_t args_start = 0;
  bool found = false;
  func_name[0] = '\0';

  size_t i = 0;
  bool in_string = false;
  char quote = '\0';

  while (i < line_len) {
    char c = line[i];

    if (in_string) {
      if (c == quote && (i == 0 || line[i - 1] != '\\')) {
        in_string = false;
      }
      i++;
      continue;
    }

    if (c == '"' || c == '\'') {
      in_string = true;
      quote = c;
      i++;
      continue;
    }

    if (c == '#') {
      break;
    }

    bool is_call_keyword =
        (i == 0 || !is_identifier_char(line[i - 1])) &&
        i + 4 <= line_len && strncmp(line + i, "call", 4) == 0 &&
        (i + 4 == line_len || isspace((unsigned char)line[i + 4]));

    if (!is_call_keyword) {
      i++;
      continue;
    }

    size_t j = i + 4;
    while (j < line_len && isspace((unsigned char)line[j])) {
      j++;
    }

    size_t name_start = j;
    while (j < line_len && is_identifier_or_dot_char(line[j])) {
      j++;
    }
    if (j <= name_start) {
      i++;
      continue;
    }

    size_t name_len = j - name_start;
    size_t with_start = j;
    while (with_start < line_len && isspace((unsigned char)line[with_start])) {
      with_start++;
    }

    bool has_with = with_start + 4 <= line_len &&
                    strncmp(line + with_start, "with", 4) == 0 &&
                    (with_start + 4 == line_len ||
                     isspace((unsigned char)line[with_start + 4]));

    size_t candidate_args_start = line_len;
    if (has_with) {
      candidate_args_start = with_start + 4;
      while (candidate_args_start < line_len &&
             isspace((unsigned char)line[candidate_args_start])) {
        candidate_args_start++;
      }
    }

    if (cursor_col >= i &&
        (best_call_start == SIZE_MAX || i >= best_call_start)) {
      size_t copy_len = name_len < func_name_size - 1 ? name_len : func_name_size - 1;
      memcpy(func_name, line + name_start, copy_len);
      func_name[copy_len] = '\0';
      args_start = candidate_args_start;
      best_call_start = i;
      found = true;
    }

    i = j;
  }

  if (!found) {
    return false;
  }

  if (cursor_col <= args_start || args_start >= line_len) {
    *active_param = 0;
    return true;
  }

  size_t limit = cursor_col < line_len ? cursor_col : line_len;
  size_t commas = 0;
  int paren_depth = 0;
  int bracket_depth = 0;
  int brace_depth = 0;
  in_string = false;
  quote = '\0';

  for (size_t pos = args_start; pos < limit; pos++) {
    char c = line[pos];
    if (in_string) {
      if (c == quote && (pos == 0 || line[pos - 1] != '\\')) {
        in_string = false;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      in_string = true;
      quote = c;
      continue;
    }
    if (c == '(') {
      paren_depth++;
      continue;
    }
    if (c == ')' && paren_depth > 0) {
      paren_depth--;
      continue;
    }
    if (c == '[') {
      bracket_depth++;
      continue;
    }
    if (c == ']' && bracket_depth > 0) {
      bracket_depth--;
      continue;
    }
    if (c == '{') {
      brace_depth++;
      continue;
    }
    if (c == '}' && brace_depth > 0) {
      brace_depth--;
      continue;
    }
    if (c == ',' && paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
      commas++;
    }
  }

  *active_param = commas;
  return true;
}

static size_t split_arguments(const char *line, size_t line_len, size_t args_start,
                              ArgSegment *segments, size_t max_segments) {
  if (!line || args_start >= line_len || !segments || max_segments == 0) {
    return 0;
  }

  size_t count = 0;
  size_t segment_start = args_start;
  bool in_string = false;
  char quote = '\0';
  int paren_depth = 0;
  int bracket_depth = 0;
  int brace_depth = 0;

  for (size_t i = args_start; i <= line_len; i++) {
    char c = i < line_len ? line[i] : ',';
    if (in_string) {
      if (c == quote && i > 0 && line[i - 1] != '\\') {
        in_string = false;
      }
      continue;
    }

    if (i < line_len && (c == '"' || c == '\'')) {
      in_string = true;
      quote = c;
      continue;
    }

    if (i < line_len && c == '(') {
      paren_depth++;
      continue;
    }
    if (i < line_len && c == ')' && paren_depth > 0) {
      paren_depth--;
      continue;
    }
    if (i < line_len && c == '[') {
      bracket_depth++;
      continue;
    }
    if (i < line_len && c == ']' && bracket_depth > 0) {
      bracket_depth--;
      continue;
    }
    if (i < line_len && c == '{') {
      brace_depth++;
      continue;
    }
    if (i < line_len && c == '}' && brace_depth > 0) {
      brace_depth--;
      continue;
    }

    if ((i == line_len || c == ',') && paren_depth == 0 && bracket_depth == 0 &&
        brace_depth == 0) {
      if (count < max_segments) {
        size_t start = segment_start;
        size_t end = i;
        while (start < end && isspace((unsigned char)line[start])) {
          start++;
        }
        while (end > start && isspace((unsigned char)line[end - 1])) {
          end--;
        }

        bool is_named = false;
        size_t p = start;
        if (p < end && is_identifier_start_char(line[p])) {
          while (p < end && is_identifier_char(line[p])) {
            p++;
          }
          while (p < end && isspace((unsigned char)line[p])) {
            p++;
          }
          if (p < end && line[p] == ':') {
            is_named = true;
          }
        }

        segments[count].start = start;
        segments[count].end = end;
        segments[count].is_named = is_named;
        count++;
      }
      segment_start = i + 1;
    }
  }

  return count;
}

static bool parse_call_edges(const char *text, CallEdge **out_edges, size_t *out_count) {
  if (!text || !out_edges || !out_count) {
    return false;
  }

  CallEdge *edges = NULL;
  size_t edge_count = 0;
  size_t edge_capacity = 0;

  FunctionContext *stack = NULL;
  size_t stack_count = 0;
  size_t stack_capacity = 0;

  size_t line_no = 0;
  const char *line_start = text;

  while (line_start && *line_start != '\0') {
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }
    size_t line_len = (size_t)(line_end - line_start);
    size_t indent = leading_indent_width(line_start, line_len);
    const char *trimmed = line_start + indent;
    size_t trimmed_len = line_len > indent ? line_len - indent : 0;
    bool is_blank = trimmed_len == 0;

    if (!is_blank) {
      while (stack_count > 0 && indent <= stack[stack_count - 1].indent) {
        stack_count--;
      }
    }

    bool is_function_def = false;
    char function_name[128] = {0};
    if (!is_blank && trimmed[0] != '#' &&
        starts_with_keyword(trimmed, "function")) {
      size_t p = 8;
      while (p < trimmed_len && isspace((unsigned char)trimmed[p])) {
        p++;
      }
      size_t start = p;
      while (p < trimmed_len && is_identifier_char(trimmed[p])) {
        p++;
      }
      if (p > start) {
        size_t name_len = p - start;
        if (name_len >= sizeof(function_name)) {
          name_len = sizeof(function_name) - 1;
        }
        memcpy(function_name, trimmed + start, name_len);
        function_name[name_len] = '\0';
        is_function_def = true;
      }
    }

    const char *caller = stack_count > 0 ? stack[stack_count - 1].name : "";

    if (!is_blank && trimmed[0] != '#') {
      bool in_string = false;
      char quote = '\0';
      for (size_t i = 0; i < trimmed_len; i++) {
        char c = trimmed[i];
        if (in_string) {
          if (c == quote && (i == 0 || trimmed[i - 1] != '\\')) {
            in_string = false;
          }
          continue;
        }
        if (c == '"' || c == '\'') {
          in_string = true;
          quote = c;
          continue;
        }
        if (c == '#') {
          break;
        }

        bool is_call = (i == 0 || !is_identifier_char(trimmed[i - 1])) &&
                       i + 4 <= trimmed_len &&
                       strncmp(trimmed + i, "call", 4) == 0 &&
                       (i + 4 == trimmed_len ||
                        isspace((unsigned char)trimmed[i + 4]));
        if (!is_call) {
          continue;
        }

        size_t j = i + 4;
        while (j < trimmed_len && isspace((unsigned char)trimmed[j])) {
          j++;
        }
        size_t name_start = j;
        while (j < trimmed_len && is_identifier_or_dot_char(trimmed[j])) {
          j++;
        }
        if (j <= name_start) {
          continue;
        }

        size_t callee_len = j - name_start;
        char callee_raw[128];
        size_t copy_len =
            callee_len < sizeof(callee_raw) - 1 ? callee_len : sizeof(callee_raw) - 1;
        memcpy(callee_raw, trimmed + name_start, copy_len);
        callee_raw[copy_len] = '\0';

        const char *normalized = strip_module_prefix(callee_raw);
        if (!normalized || normalized[0] == '\0') {
          continue;
        }

        if (edge_count >= edge_capacity) {
          size_t new_capacity = edge_capacity == 0 ? 32 : edge_capacity * 2;
          CallEdge *grown = realloc(edges, new_capacity * sizeof(*edges));
          if (!grown) {
            free(edges);
            free(stack);
            return false;
          }
          edges = grown;
          edge_capacity = new_capacity;
        }

        memset(&edges[edge_count], 0, sizeof(edges[edge_count]));
        strncpy(edges[edge_count].caller, caller, sizeof(edges[edge_count].caller) - 1);
        strncpy(edges[edge_count].callee, normalized,
                sizeof(edges[edge_count].callee) - 1);
        edges[edge_count].line = line_no;
        edges[edge_count].col = indent + name_start;
        edges[edge_count].length = strlen(normalized);
        edge_count++;
      }
    }

    if (is_function_def) {
      if (stack_count >= stack_capacity) {
        size_t new_capacity = stack_capacity == 0 ? 8 : stack_capacity * 2;
        FunctionContext *grown = realloc(stack, new_capacity * sizeof(*stack));
        if (!grown) {
          free(edges);
          free(stack);
          return false;
        }
        stack = grown;
        stack_capacity = new_capacity;
      }
      memset(&stack[stack_count], 0, sizeof(stack[stack_count]));
      strncpy(stack[stack_count].name, function_name,
              sizeof(stack[stack_count].name) - 1);
      stack[stack_count].indent = indent;
      stack_count++;
    }

    if (*line_end == '\n') {
      line_start = line_end + 1;
    } else {
      break;
    }
    line_no++;
  }

  free(stack);
  *out_edges = edges;
  *out_count = edge_count;
  return true;
}

static bool append_call_hierarchy_item(char **json, size_t *capacity, size_t *len,
                                       const Symbol *sym) {
  if (!json || !capacity || !len || !sym || !sym->name || !g_doc || !g_doc->uri) {
    return false;
  }

  char escaped_name[LSP_PATTERN_BUFFER_SIZE];
  char escaped_uri[LSP_PATTERN_BUFFER_SIZE];
  json_escape(sym->name, escaped_name, sizeof(escaped_name));
  json_escape(g_doc->uri, escaped_uri, sizeof(escaped_uri));

  size_t line = sym->line > 0 ? sym->line - 1 : 0;
  size_t col = sym->column > 0 ? sym->column - 1 : 0;
  size_t end_col = col + strlen(sym->name);

  return append_jsonf(
      json, capacity, len,
      "{\"name\":\"%s\",\"kind\":12,\"uri\":\"%s\","
      "\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}},"
      "\"selectionRange\":{\"start\":{\"line\":%zu,\"character\":%zu},"
      "\"end\":{\"line\":%zu,\"character\":%zu}},"
      "\"data\":{\"name\":\"%s\"}}",
      escaped_name, escaped_uri, line, col, line, end_col, line, col, line,
      end_col, escaped_name);
}

void handle_semantic_tokens(const char *id) {
  if (!g_doc || !g_doc->text) {
    send_response(id, "{\"data\":[]}");
    return;
  }

  SemanticTokenEntry *tokens = NULL;
  size_t token_count = 0;
  size_t token_capacity = 0;

  bool ok = add_symbol_semantic_tokens(&tokens, &token_count, &token_capacity) &&
            add_lexical_semantic_tokens(&tokens, &token_count, &token_capacity);
  if (!ok) {
    free(tokens);
    send_response(id, "{\"data\":[]}");
    return;
  }

  qsort(tokens, token_count, sizeof(*tokens), semantic_token_cmp);

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool append_ok = append_jsonf(&json, &capacity, &len, "{\"data\":[");

  int prev_line = 0;
  int prev_col = 0;
  bool first = true;

  for (size_t i = 0; append_ok && i < token_count; i++) {
    if (tokens[i].line < 0 || tokens[i].col < 0 || tokens[i].length <= 0) {
      continue;
    }

    int delta_line = tokens[i].line - prev_line;
    int delta_col = delta_line == 0 ? tokens[i].col - prev_col : tokens[i].col;
    if (delta_line < 0 || delta_col < 0) {
      continue;
    }

    append_ok = append_jsonf(&json, &capacity, &len, "%s%d,%d,%d,%d,%d",
                             first ? "" : ",", delta_line, delta_col,
                             tokens[i].length, tokens[i].type,
                             tokens[i].modifiers);
    first = false;
    prev_line = tokens[i].line;
    prev_col = tokens[i].col;
  }

  append_ok = append_ok && append_jsonf(&json, &capacity, &len, "]}");

  if (!append_ok || !json) {
    free(tokens);
    free(json);
    send_response(id, "{\"data\":[]}");
    return;
  }

  send_response(id, json);
  free(tokens);
  free(json);
}

void handle_signature_help(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "null");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str = json_get_nested_value(body, "params.position.character");
  if (!line_str || !character_str) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }

  size_t line = 0;
  size_t character = 0;
  bool parsed =
      safe_strtoul(line_str, &line) && safe_strtoul(character_str, &character);
  free(line_str);
  free(character_str);
  if (!parsed) {
    send_response(id, "null");
    return;
  }

  const char *line_start = NULL;
  size_t line_len = 0;
  if (!get_line_span(g_doc->text, line, &line_start, &line_len)) {
    send_response(id, "null");
    return;
  }

  char function_name[128];
  size_t active_param = 0;
  if (!parse_call_context(line_start, line_len, character, function_name,
                          sizeof(function_name), &active_param)) {
    send_response(id, "null");
    return;
  }

  Symbol *function_sym = find_function_symbol(strip_module_prefix(function_name));
  const BuiltinSignature *builtin = find_builtin_signature(function_name);
  if (!function_sym && !builtin) {
    int builtin_count = get_builtin_arg_count(function_name);
    if (builtin_count < 0) {
      send_response(id, "null");
      return;
    }
  }

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len,
                         "{\"signatures\":[{\"label\":\"");
  if (!ok) {
    free(json);
    send_response(id, "null");
    return;
  }

  if (function_sym) {
    char label[512];
    size_t pos = 0;
    pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "%s(",
                            function_sym->name);
    for (size_t i = 0; i < function_sym->param_count && pos < sizeof(label); i++) {
      if (i > 0) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, ", ");
      }
      const char *pname =
          function_sym->param_names && function_sym->param_names[i]
              ? function_sym->param_names[i]
              : "arg";
      if (function_sym->has_variadic && i == function_sym->param_count - 1) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "...%s", pname);
      } else if (i >= function_sym->required_param_count) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "%s=...", pname);
      } else {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "%s", pname);
      }
    }
    snprintf(label + (pos < sizeof(label) ? pos : sizeof(label) - 1),
             pos < sizeof(label) ? sizeof(label) - pos : 1, ")");

    char escaped_label[1024];
    json_escape(label, escaped_label, sizeof(escaped_label));
    ok = append_jsonf(&json, &capacity, &len, "%s\",", escaped_label);

    if (ok) {
      ok = append_jsonf(&json, &capacity, &len,
                        "\"documentation\":\"User-defined function\","
                        "\"parameters\":[");
    }

    for (size_t i = 0; ok && i < function_sym->param_count; i++) {
      const char *pname =
          function_sym->param_names && function_sym->param_names[i]
              ? function_sym->param_names[i]
              : "arg";
      char escaped_param[256];
      json_escape(pname, escaped_param, sizeof(escaped_param));
      ok = append_jsonf(&json, &capacity, &len, "%s{\"label\":\"%s\"}",
                        i == 0 ? "" : ",", escaped_param);
    }
  } else if (builtin) {
    char label[512];
    size_t pos = (size_t)snprintf(label, sizeof(label), "%s(", function_name);
    for (size_t i = 0; i < builtin->param_count && pos < sizeof(label); i++) {
      if (i > 0) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, ", ");
      }
      if (builtin->variadic && i == builtin->param_count - 1) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "...%s",
                                builtin->params[i]);
      } else {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "%s",
                                builtin->params[i]);
      }
    }
    snprintf(label + (pos < sizeof(label) ? pos : sizeof(label) - 1),
             pos < sizeof(label) ? sizeof(label) - pos : 1, ")");

    char escaped_label[1024];
    char escaped_doc[1024];
    json_escape(label, escaped_label, sizeof(escaped_label));
    json_escape(builtin->documentation, escaped_doc, sizeof(escaped_doc));
    ok = append_jsonf(&json, &capacity, &len,
                      "%s\",\"documentation\":\"%s\",\"parameters\":[",
                      escaped_label, escaped_doc);

    for (size_t i = 0; ok && i < builtin->param_count; i++) {
      char escaped_param[256];
      json_escape(builtin->params[i], escaped_param, sizeof(escaped_param));
      ok = append_jsonf(&json, &capacity, &len, "%s{\"label\":\"%s\"}",
                        i == 0 ? "" : ",", escaped_param);
    }
  } else {
    int builtin_count = get_builtin_arg_count(function_name);
    size_t generic_count = builtin_count < 0 ? 1 : (size_t)builtin_count;
    if (builtin_count == -2) {
      generic_count = 1;
    }

    char label[256];
    size_t pos = (size_t)snprintf(label, sizeof(label), "%s(", function_name);
    for (size_t i = 0; i < generic_count && pos < sizeof(label); i++) {
      if (i > 0) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, ", ");
      }
      if (builtin_count == -2 && i == generic_count - 1) {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "...arg%zu",
                                i + 1);
      } else {
        pos += (size_t)snprintf(label + pos, sizeof(label) - pos, "arg%zu",
                                i + 1);
      }
    }
    snprintf(label + (pos < sizeof(label) ? pos : sizeof(label) - 1),
             pos < sizeof(label) ? sizeof(label) - pos : 1, ")");

    char escaped_label[512];
    json_escape(label, escaped_label, sizeof(escaped_label));
    ok = append_jsonf(&json, &capacity, &len,
                      "%s\",\"documentation\":\"Built-in function\","
                      "\"parameters\":[",
                      escaped_label);

    for (size_t i = 0; ok && i < generic_count; i++) {
      ok = append_jsonf(&json, &capacity, &len,
                        "%s{\"label\":\"arg%zu\"}", i == 0 ? "" : ",", i + 1);
    }
  }

  size_t param_count = 0;
  bool variadic = false;
  if (function_sym) {
    param_count = function_sym->param_count;
    variadic = function_sym->has_variadic;
  } else if (builtin) {
    param_count = builtin->param_count;
    variadic = builtin->variadic;
  } else {
    int builtin_count = get_builtin_arg_count(function_name);
    if (builtin_count == -2) {
      param_count = 1;
      variadic = true;
    } else if (builtin_count > 0) {
      param_count = (size_t)builtin_count;
    }
  }

  if (param_count > 0 && active_param >= param_count) {
    active_param = variadic ? param_count - 1 : param_count - 1;
  } else if (param_count == 0) {
    active_param = 0;
  }

  ok = ok && append_jsonf(&json, &capacity, &len,
                          "]"
                          "}],\"activeSignature\":0,\"activeParameter\":%zu}",
                          active_param);

  if (!ok || !json) {
    free(json);
    send_response(id, "null");
    return;
  }

  send_response(id, json);
  free(json);
}

void handle_inlay_hints(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "[]");
    return;
  }

  size_t start_line = 0;
  size_t end_line = SIZE_MAX;
  char *start_line_str = json_get_nested_value(body, "params.range.start.line");
  char *end_line_str = json_get_nested_value(body, "params.range.end.line");
  if (start_line_str) {
    safe_strtoul(start_line_str, &start_line);
  }
  if (end_line_str) {
    safe_strtoul(end_line_str, &end_line);
  }
  free(start_line_str);
  free(end_line_str);

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len, "[");
  bool first = true;

  const char *line_start = g_doc->text;
  size_t line_no = 0;
  while (line_start && *line_start != '\0' && ok) {
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }
    size_t line_len = (size_t)(line_end - line_start);

    if (line_no >= start_line && line_no <= end_line) {
      bool in_string = false;
      char quote = '\0';
      for (size_t i = 0; i < line_len && ok; i++) {
        char c = line_start[i];
        if (in_string) {
          if (c == quote && (i == 0 || line_start[i - 1] != '\\')) {
            in_string = false;
          }
          continue;
        }
        if (c == '"' || c == '\'') {
          in_string = true;
          quote = c;
          continue;
        }
        if (c == '#') {
          break;
        }

        bool is_call = (i == 0 || !is_identifier_char(line_start[i - 1])) &&
                       i + 4 <= line_len &&
                       strncmp(line_start + i, "call", 4) == 0 &&
                       (i + 4 == line_len ||
                        isspace((unsigned char)line_start[i + 4]));
        if (!is_call) {
          continue;
        }

        size_t j = i + 4;
        while (j < line_len && isspace((unsigned char)line_start[j])) {
          j++;
        }
        size_t name_start = j;
        while (j < line_len && is_identifier_or_dot_char(line_start[j])) {
          j++;
        }
        if (j <= name_start) {
          continue;
        }

        char function_name[128];
        size_t name_len = j - name_start;
        size_t copy_len =
            name_len < sizeof(function_name) - 1 ? name_len : sizeof(function_name) - 1;
        memcpy(function_name, line_start + name_start, copy_len);
        function_name[copy_len] = '\0';

        while (j < line_len && isspace((unsigned char)line_start[j])) {
          j++;
        }
        bool has_with = j + 4 <= line_len &&
                        strncmp(line_start + j, "with", 4) == 0 &&
                        (j + 4 == line_len ||
                         isspace((unsigned char)line_start[j + 4]));
        if (!has_with) {
          continue;
        }
        size_t args_start = j + 4;
        while (args_start < line_len &&
               isspace((unsigned char)line_start[args_start])) {
          args_start++;
        }
        if (args_start >= line_len) {
          continue;
        }

        const char *param_names[16];
        size_t param_count = 0;

        Symbol *sym = find_function_symbol(strip_module_prefix(function_name));
        if (sym && sym->param_names) {
          param_count = sym->param_count > 16 ? 16 : sym->param_count;
          for (size_t p = 0; p < param_count; p++) {
            param_names[p] =
                sym->param_names[p] ? sym->param_names[p] : "arg";
          }
        } else {
          const BuiltinSignature *builtin = find_builtin_signature(function_name);
          if (builtin) {
            param_count = builtin->param_count > 16 ? 16 : builtin->param_count;
            for (size_t p = 0; p < param_count; p++) {
              param_names[p] = builtin->params[p];
            }
          }
        }

        if (param_count == 0) {
          continue;
        }

        ArgSegment segments[32];
        size_t segment_count =
            split_arguments(line_start, line_len, args_start, segments, 32);
        size_t positional_index = 0;

        for (size_t s = 0; s < segment_count && ok; s++) {
          if (segments[s].start >= segments[s].end) {
            continue;
          }
          if (segments[s].is_named) {
            continue;
          }

          size_t param_index =
              positional_index < param_count ? positional_index : param_count - 1;
          positional_index++;

          char label[256];
          snprintf(label, sizeof(label), "%s:", param_names[param_index]);
          char escaped_label[512];
          json_escape(label, escaped_label, sizeof(escaped_label));

          ok = append_jsonf(
              &json, &capacity, &len,
              "%s{\"position\":{\"line\":%zu,\"character\":%zu},"
              "\"label\":\"%s\",\"kind\":2,\"paddingRight\":true}",
              first ? "" : ",", line_no, segments[s].start, escaped_label);
          first = false;
        }
      }
    }

    if (*line_end == '\n') {
      line_start = line_end + 1;
      line_no++;
    } else {
      break;
    }
  }

  ok = ok && append_jsonf(&json, &capacity, &len, "]");
  if (!ok || !json) {
    free(json);
    send_response(id, "[]");
    return;
  }

  send_response(id, json);
  free(json);
}

void handle_prepare_call_hierarchy(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "[]");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str = json_get_nested_value(body, "params.position.character");
  if (!line_str || !character_str) {
    free(line_str);
    free(character_str);
    send_response(id, "[]");
    return;
  }

  size_t line = 0;
  size_t character = 0;
  bool parsed =
      safe_strtoul(line_str, &line) && safe_strtoul(character_str, &character);
  free(line_str);
  free(character_str);
  if (!parsed) {
    send_response(id, "[]");
    return;
  }

  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "[]");
    return;
  }

  const char *target_name = strip_module_prefix(word);
  Symbol *sym = find_function_symbol(target_name);
  free(word);

  if (!sym) {
    send_response(id, "[]");
    return;
  }

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len, "[") &&
            append_call_hierarchy_item(&json, &capacity, &len, sym) &&
            append_jsonf(&json, &capacity, &len, "]");
  if (!ok || !json) {
    free(json);
    send_response(id, "[]");
    return;
  }

  send_response(id, json);
  free(json);
}

void handle_call_hierarchy_incoming(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "[]");
    return;
  }

  char *target_name_json = json_get_nested_value(body, "params.item.data.name");
  if (!target_name_json) {
    target_name_json = json_get_nested_value(body, "params.item.name");
  }
  if (!target_name_json) {
    send_response(id, "[]");
    return;
  }

  const char *target_name = strip_module_prefix(target_name_json);
  Symbol *target_sym = find_function_symbol(target_name);
  free(target_name_json);
  if (!target_sym) {
    send_response(id, "[]");
    return;
  }

  CallEdge *edges = NULL;
  size_t edge_count = 0;
  if (!parse_call_edges(g_doc->text, &edges, &edge_count)) {
    send_response(id, "[]");
    return;
  }

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len, "[");
  bool first_entry = true;

  for (Symbol *caller = g_doc->symbols; ok && caller; caller = caller->next) {
    if (caller->type != SYMBOL_FUNCTION || !caller->name) {
      continue;
    }

    size_t match_count = 0;
    for (size_t i = 0; i < edge_count; i++) {
      if (strcmp(edges[i].caller, caller->name) == 0 &&
          strcmp(edges[i].callee, target_sym->name) == 0) {
        match_count++;
      }
    }
    if (match_count == 0) {
      continue;
    }

    ok = append_jsonf(&json, &capacity, &len, "%s{\"from\":",
                      first_entry ? "" : ",");
    ok = ok && append_call_hierarchy_item(&json, &capacity, &len, caller);
    ok = ok && append_jsonf(&json, &capacity, &len, ",\"fromRanges\":[");

    bool first_range = true;
    for (size_t i = 0; ok && i < edge_count; i++) {
      if (strcmp(edges[i].caller, caller->name) != 0 ||
          strcmp(edges[i].callee, target_sym->name) != 0) {
        continue;
      }
      ok = append_jsonf(
          &json, &capacity, &len,
          "%s{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}}",
          first_range ? "" : ",", edges[i].line, edges[i].col, edges[i].line,
          edges[i].col + edges[i].length);
      first_range = false;
    }

    ok = ok && append_jsonf(&json, &capacity, &len, "]}");
    first_entry = false;
  }

  ok = ok && append_jsonf(&json, &capacity, &len, "]");
  free(edges);

  if (!ok || !json) {
    free(json);
    send_response(id, "[]");
    return;
  }

  send_response(id, json);
  free(json);
}

void handle_call_hierarchy_outgoing(const char *id, const char *body) {
  if (!g_doc || !g_doc->text || !body) {
    send_response(id, "[]");
    return;
  }

  char *source_name_json = json_get_nested_value(body, "params.item.data.name");
  if (!source_name_json) {
    source_name_json = json_get_nested_value(body, "params.item.name");
  }
  if (!source_name_json) {
    send_response(id, "[]");
    return;
  }

  const char *source_name = strip_module_prefix(source_name_json);
  Symbol *source_sym = find_function_symbol(source_name);
  free(source_name_json);
  if (!source_sym) {
    send_response(id, "[]");
    return;
  }

  CallEdge *edges = NULL;
  size_t edge_count = 0;
  if (!parse_call_edges(g_doc->text, &edges, &edge_count)) {
    send_response(id, "[]");
    return;
  }

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len, "[");
  bool first_entry = true;

  for (Symbol *callee = g_doc->symbols; ok && callee; callee = callee->next) {
    if (callee->type != SYMBOL_FUNCTION || !callee->name ||
        strcmp(callee->name, source_sym->name) == 0) {
      continue;
    }

    size_t match_count = 0;
    for (size_t i = 0; i < edge_count; i++) {
      if (strcmp(edges[i].caller, source_sym->name) == 0 &&
          strcmp(edges[i].callee, callee->name) == 0) {
        match_count++;
      }
    }
    if (match_count == 0) {
      continue;
    }

    ok = append_jsonf(&json, &capacity, &len, "%s{\"to\":",
                      first_entry ? "" : ",");
    ok = ok && append_call_hierarchy_item(&json, &capacity, &len, callee);
    ok = ok && append_jsonf(&json, &capacity, &len, ",\"fromRanges\":[");

    bool first_range = true;
    for (size_t i = 0; ok && i < edge_count; i++) {
      if (strcmp(edges[i].caller, source_sym->name) != 0 ||
          strcmp(edges[i].callee, callee->name) != 0) {
        continue;
      }
      ok = append_jsonf(
          &json, &capacity, &len,
          "%s{\"start\":{\"line\":%zu,\"character\":%zu},"
          "\"end\":{\"line\":%zu,\"character\":%zu}}",
          first_range ? "" : ",", edges[i].line, edges[i].col, edges[i].line,
          edges[i].col + edges[i].length);
      first_range = false;
    }

    ok = ok && append_jsonf(&json, &capacity, &len, "]}");
    first_entry = false;
  }

  ok = ok && append_jsonf(&json, &capacity, &len, "]");
  free(edges);

  if (!ok || !json) {
    free(json);
    send_response(id, "[]");
    return;
  }

  send_response(id, json);
  free(json);
}

void handle_folding_range(const char *id,
                                const char *body __attribute__((unused))) {
  if (!g_doc || !g_doc->text) {
    send_response(id, "[]");
    return;
  }

  char *json = NULL;
  size_t capacity = 0;
  size_t len = 0;
  bool ok = append_jsonf(&json, &capacity, &len, "[");
  bool first = true;

  const char *text = g_doc->text;
  const char *line_starts[4096];
  size_t line_lengths[4096];
  size_t line_count = 0;

  const char *line_start = text;
  while (line_start && *line_start != '\0' && line_count < 4096) {
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }
    line_starts[line_count] = line_start;
    line_lengths[line_count] = (size_t)(line_end - line_start);
    line_count++;
    if (*line_end == '\n') {
      line_start = line_end + 1;
    } else {
      break;
    }
  }

  for (size_t i = 0; ok && i < line_count; i++) {
    const char *line = line_starts[i];
    size_t line_len = line_lengths[i];
    size_t indent = leading_indent_width(line, line_len);
    const char *trimmed = line + indent;
    size_t trimmed_len = line_len > indent ? line_len - indent : 0;

    if (trimmed_len == 0) {
      continue;
    }

    if (trimmed[0] == '#') {
      size_t j = i + 1;
      while (j < line_count) {
        size_t next_indent = leading_indent_width(line_starts[j], line_lengths[j]);
        size_t next_trimmed_len =
            line_lengths[j] > next_indent ? line_lengths[j] - next_indent : 0;
        if (next_trimmed_len == 0 || line_starts[j][next_indent] != '#') {
          break;
        }
        j++;
      }
      if (j > i + 1) {
        ok = append_jsonf(&json, &capacity, &len,
                          "%s{\"startLine\":%zu,\"endLine\":%zu,\"kind\":\"comment\"}",
                          first ? "" : ",", i, j - 1);
        first = false;
      }
      i = j > 0 ? j - 1 : i;
      continue;
    }

    size_t trimmed_end = line_len;
    while (trimmed_end > 0 &&
           isspace((unsigned char)line[trimmed_end - 1])) {
      trimmed_end--;
    }
    if (trimmed_end == 0 || line[trimmed_end - 1] != ':') {
      continue;
    }

    size_t last_content_line = i;
    size_t j = i + 1;
    for (; j < line_count; j++) {
      size_t next_indent = leading_indent_width(line_starts[j], line_lengths[j]);
      size_t next_trimmed_len =
          line_lengths[j] > next_indent ? line_lengths[j] - next_indent : 0;
      if (next_trimmed_len == 0) {
        continue;
      }
      if (next_indent <= indent) {
        break;
      }
      last_content_line = j;
    }

    if (last_content_line > i) {
      ok = append_jsonf(&json, &capacity, &len,
                        "%s{\"startLine\":%zu,\"endLine\":%zu,\"kind\":\"region\"}",
                        first ? "" : ",", i, last_content_line);
      first = false;
    }
  }

  ok = ok && append_jsonf(&json, &capacity, &len, "]");

  if (!ok || !json) {
    free(json);
    send_response(id, "[]");
    return;
  }

  send_response(id, json);
  free(json);
}
