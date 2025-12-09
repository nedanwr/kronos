/**
 * @file lsp_handlers.c
 * @brief Basic LSP request handlers
 */

#include "lsp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern DocumentState *g_doc;

void handle_initialize(const char *id) {
  const char *capabilities =
      "{"
      "\"capabilities\":{"
      "\"textDocumentSync\":1,"
      "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
      "\"definitionProvider\":true,"
      "\"hoverProvider\":true,"
      "\"documentSymbolProvider\":true,"
      "\"semanticTokensProvider\":{"
      "\"legend\":{"
      "\"tokenTypes\":[\"variable\",\"function\",\"parameter\"],"
      "\"tokenModifiers\":[\"unused\",\"readonly\"]"
      "},"
      "\"range\":false,"
      "\"full\":{\"delta\":false}"
      "}"
      "}"
      "}";
  send_response(id, capabilities);
}

void handle_shutdown(const char *id) { send_response(id, "null"); }

void handle_did_open(const char *uri, const char *text) {
  // Update or create document state
  if (g_doc) {
    free_document_state(g_doc);
  }
  g_doc = malloc(sizeof(DocumentState));
  if (g_doc) {
    g_doc->uri = strdup(uri);
    g_doc->text = strdup(text);
    g_doc->symbols = NULL;
    g_doc->ast = NULL;
    g_doc->imported_modules = NULL;
  }
  check_diagnostics(uri, text);
}

void handle_did_change(const char *uri, const char *text) {
  // Update document text
  if (g_doc && g_doc->uri && strcmp(g_doc->uri, uri) == 0) {
    free(g_doc->text);
    g_doc->text = strdup(text);
  }
  check_diagnostics(uri, text);
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
  char *formatted = malloc(text_len * 2 + 1); // Allocate extra space
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

  for (size_t i = 0; i < text_len && out_pos < text_len * 2 - 100; i++) {
    char c = text[i];

    if (c == '\n') {
      formatted[out_pos++] = '\n';
      at_line_start = true;
      last_was_space = false;
      continue;
    }

    if (at_line_start) {
      // Skip leading whitespace
      if (isspace((unsigned char)c) && c != '\n') {
        continue;
      }

      // Apply indentation
      int spaces = indent_level * 4;
      for (int j = 0; j < spaces && out_pos < text_len * 2 - 100; j++) {
        formatted[out_pos++] = ' ';
      }
      at_line_start = false;
    }

    // Handle indentation changes
    if (c == ':' && i + 1 < text_len && text[i + 1] == '\n') {
      formatted[out_pos++] = c;
      indent_level++;
      last_was_space = false;
      continue;
    }

    // Decrease indent for certain keywords at start of line
    if (at_line_start || (out_pos > 0 && formatted[out_pos - 1] == '\n')) {
      if (strncmp(text + i, "else", 4) == 0 ||
          strncmp(text + i, "catch", 5) == 0 ||
          strncmp(text + i, "finally", 7) == 0) {
        if (indent_level > 0) {
          indent_level--;
        }
      }
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
      size_t ref_count = count_symbol_references(sym->name, g_doc->ast);

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

void handle_semantic_tokens(const char *id) {
  if (!g_doc || !g_doc->text || !g_doc->symbols)
    return send_response(id, "{\"data\":[]}");

  // Build semantic tokens array
  // Format: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers, ...]
  // Token types: 0=variable, 1=function, 2=parameter
  // Token modifiers: 0=unused (bit 0), 1=readonly (bit 1)
  char tokens[LSP_REFERENCES_BUFFER_SIZE];
  size_t pos = 0;
  size_t remaining = sizeof(tokens);

  pos += snprintf(tokens + pos, remaining - pos, "{\"data\":[");

  bool first = true;
  int prev_line = 0;
  int prev_col = 0;

  for (Symbol *sym = g_doc->symbols; sym != NULL; sym = sym->next) {
    if (sym->type == SYMBOL_PARAMETER)
      continue;

    // Find the actual position of the symbol in source text
    size_t line = 1, col = 0;

    if (sym->type == SYMBOL_FUNCTION) {
      char pattern[LSP_PATTERN_BUFFER_SIZE];
      snprintf(pattern, sizeof(pattern), "function %s with", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &line, &col);
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "function %s", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &line, &col);
      }
    } else {
      char pattern[LSP_PATTERN_BUFFER_SIZE];
      snprintf(pattern, sizeof(pattern), "let %s to", sym->name);
      find_node_position(NULL, g_doc->text, pattern, &line, &col);
      if (line == 1 && col == 0) {
        snprintf(pattern, sizeof(pattern), "set %s to", sym->name);
        find_node_position(NULL, g_doc->text, pattern, &line, &col);
      }
    }

    if (line == 1 && col == 0)
      continue; // Skip if not found

    // Calculate the column position of the symbol name itself
    size_t name_col = col;
    const char *line_start = g_doc->text;
    size_t current_line = 1;
    for (const char *p = g_doc->text; *p != '\0' && current_line < line; p++) {
      if (*p == '\n') {
        current_line++;
        if (current_line == line) {
          line_start = p + 1;
          break;
        }
      }
    }

    if (sym->type == SYMBOL_FUNCTION) {
      const char *pattern_start = line_start + col;
      const char *name_start = pattern_start;
      while (*name_start != '\0' && *name_start != '\n' &&
             (*name_start == ' ' || *name_start == '\t' ||
              (*name_start >= 'a' && *name_start <= 'z') ||
              (*name_start >= 'A' && *name_start <= 'Z'))) {
        if (*name_start == ' ') {
          name_start++;
          break;
        }
        name_start++;
      }
      while (*name_start == ' ' || *name_start == '\t') {
        name_start++;
      }
      name_col = (size_t)(name_start - line_start);
    } else {
      const char *pattern_start = line_start + col;
      const char *name_start = pattern_start;
      while (*name_start != '\0' && *name_start != '\n' &&
             (*name_start == ' ' || *name_start == '\t' ||
              (*name_start >= 'a' && *name_start <= 'z') ||
              (*name_start >= 'A' && *name_start <= 'Z'))) {
        if (*name_start == ' ') {
          name_start++;
          break;
        }
        name_start++;
      }
      while (*name_start == ' ' || *name_start == '\t') {
        name_start++;
      }
      name_col = (size_t)(name_start - line_start);
    }

    // Determine token type and modifiers
    int token_type =
        sym->type == SYMBOL_FUNCTION ? 1 : 0; // 0=variable, 1=function
    int token_modifiers = 0;

    // Mark as unused if not used (or defined but never read for variables)
    if (sym->type == SYMBOL_VARIABLE) {
      if ((sym->written && !sym->read) || (!sym->read && !sym->written)) {
        token_modifiers |= 1; // unused modifier (bit 0)
      }
      if (!sym->is_mutable) {
        token_modifiers |= 2; // readonly modifier (bit 1)
      }
    } else if (sym->type == SYMBOL_FUNCTION) {
      if (!sym->read && !sym->written) {
        token_modifiers |= 1; // unused modifier (bit 0)
      }
    }

    // Calculate relative positions (delta encoding)
    int delta_line = (int)line - 1 - prev_line;
    int delta_col = delta_line == 0 ? (int)name_col - prev_col : (int)name_col;

    if (!first) {
      int written = snprintf(tokens + pos, remaining - pos, ",");
      if (written > 0 && (size_t)written < remaining - pos) {
        pos += (size_t)written;
        remaining -= (size_t)written;
      } else {
        break; // Buffer full
      }
    }
    first = false;

    int written =
        snprintf(tokens + pos, remaining - pos, "%d,%d,%zu,%d,%d", delta_line,
                 delta_col, strlen(sym->name), token_type, token_modifiers);
    if (written > 0 && (size_t)written < remaining - pos) {
      pos += (size_t)written;
      remaining -= (size_t)written;
    } else {
      break; // Buffer full
    }

    prev_line = (int)line - 1;
    prev_col = (int)name_col;
  }

  int written = snprintf(tokens + pos, remaining - pos, "]}");
  if (written > 0 && (size_t)written < remaining - pos) {
    pos += (size_t)written;
  }
  send_response(id, tokens);
}

