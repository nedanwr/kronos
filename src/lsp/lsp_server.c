/*
 * Kronos Language Server Protocol Implementation (STUB)
 *
 * This is a minimal LSP server stub providing basic completion support.
 * It uses simplified string-based parsing and hardcoded values for prototyping.
 *
 * Current capabilities:
 * - Keyword completion (set, let, if, for, while, and, or, not, list, at, from, end, len, function, etc.)
 * - Basic text synchronization (didOpen/didChange tracked but not parsed)
 *
 * Missing critical features (see MISSING_FEATURES.md):
 * - Proper JSON-RPC request/response parsing
 * - Accurate diagnostics with real line/column numbers
 * - Go-to-definition, find references, hover, rename
 * - Document symbols, code actions, formatting
 *
 * Usage: ./kronos-lsp
 * Communicates via stdin/stdout using JSON-RPC 2.0
 */

#include "../frontend/parser.h"
#include "../frontend/tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Helper utilities for naive JSON parsing (sufficient for stub LSP server)
static const char *skip_ws(const char *s) {
  while (s && *s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

static const char *find_value_start(const char *json, const char *key) {
  if (!json || !key)
    return NULL;

  char pattern[128];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  if (written <= 0 || (size_t)written >= sizeof(pattern))
    return NULL;

  const char *pos = json;
  size_t pattern_len = (size_t)written;
  while ((pos = strstr(pos, pattern)) != NULL) {
    pos += pattern_len;
    pos = skip_ws(pos);
    if (*pos != ':')
      continue;
    pos++;
    return skip_ws(pos);
  }
  return NULL;
}

static char *json_get_string_value(const char *json, const char *key) {
  const char *value_start = find_value_start(json, key);
  if (!value_start || *value_start != '"')
    return NULL;

  value_start++; // skip opening quote
  const char *cursor = value_start;
  while (*cursor) {
    if (*cursor == '"') {
      const char *back = cursor - 1;
      bool escaped = false;
      while (back >= value_start && *back == '\\') {
        escaped = !escaped;
        back--;
      }
      if (!escaped)
        break;
    }
    cursor++;
  }
  if (*cursor != '"')
    return NULL;

  size_t raw_len = (size_t)(cursor - value_start);
  char *result = malloc(raw_len + 1);
  if (!result)
    return NULL;

  size_t out_idx = 0;
  for (const char *iter = value_start; iter < cursor; ++iter) {
    if (*iter == '\\' && (iter + 1) < cursor) {
      ++iter;
      switch (*iter) {
      case '"':
        result[out_idx++] = '"';
        break;
      case '\\':
        result[out_idx++] = '\\';
        break;
      case '/':
        result[out_idx++] = '/';
        break;
      case 'b':
        result[out_idx++] = '\b';
        break;
      case 'f':
        result[out_idx++] = '\f';
        break;
      case 'n':
        result[out_idx++] = '\n';
        break;
      case 'r':
        result[out_idx++] = '\r';
        break;
      case 't':
        result[out_idx++] = '\t';
        break;
      default:
        result[out_idx++] = *iter;
        break;
      }
    } else {
      result[out_idx++] = *iter;
    }
  }
  result[out_idx] = '\0';
  return result;
}

static char *json_get_unquoted_value(const char *json, const char *key) {
  const char *value_start = find_value_start(json, key);
  if (!value_start || *value_start == '"' || *value_start == '\0')
    return NULL;

  const char *cursor = value_start;
  while (*cursor && !isspace((unsigned char)*cursor) && *cursor != ',' &&
         *cursor != '}' && *cursor != ']') {
    cursor++;
  }

  size_t len = (size_t)(cursor - value_start);
  if (len == 0)
    return NULL;

  char *result = malloc(len + 1);
  if (!result)
    return NULL;
  memcpy(result, value_start, len);
  result[len] = '\0';
  return result;
}

static char *json_get_id_value(const char *json) {
  char *string_id = json_get_string_value(json, "id");
  if (string_id) {
    size_t len = strlen(string_id);
    char *wrapped = malloc(len + 3);
    if (!wrapped) {
      free(string_id);
      return NULL;
    }
    wrapped[0] = '"';
    memcpy(wrapped + 1, string_id, len);
    wrapped[len + 1] = '"';
    wrapped[len + 2] = '\0';
    free(string_id);
    return wrapped;
  }
  return json_get_unquoted_value(json, "id");
}

static bool read_lsp_message(char **out_body, size_t *out_length) {
  if (!out_body || !out_length)
    return false;

  char header_line[1024];
  size_t content_length = 0;
  bool have_length = false;

  while (fgets(header_line, sizeof(header_line), stdin)) {
    if (header_line[0] == '\r' || header_line[0] == '\n') {
      break;
    }

    if (strncasecmp(header_line, "Content-Length:", 15) == 0) {
      content_length = (size_t)strtoul(header_line + 15, NULL, 10);
      have_length = true;
    }
  }

  if (!have_length) {
    return false;
  }

  char *body = malloc(content_length + 1);
  if (!body) {
    fprintf(stderr, "LSP server: failed to allocate %zu-byte body buffer\n",
            content_length);
    return false;
  }

  size_t read_total = 0;
  while (read_total < content_length) {
    size_t bytes_read =
        fread(body + read_total, 1, content_length - read_total, stdin);
    if (bytes_read == 0) {
      free(body);
      return false;
    }
    read_total += bytes_read;
  }

  body[content_length] = '\0';
  *out_body = body;
  *out_length = content_length;
  return true;
}

// JSON-escape a string (escape backslashes, quotes, and control characters)
static void json_escape(const char *input, char *output, size_t output_size) {
  size_t out_pos = 0;
  for (size_t i = 0; input[i] != '\0' && out_pos < output_size - 1; i++) {
    switch (input[i]) {
    case '\\':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '\\';
      }
      break;
    case '"':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '"';
      }
      break;
    case '\n':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 'n';
      }
      break;
    case '\r':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 'r';
      }
      break;
    case '\t':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = 't';
      }
      break;
    default:
      // Escape other control characters (0x00-0x1F)
      if ((unsigned char)input[i] < 0x20) {
        if (out_pos < output_size - 6) {
          snprintf(output + out_pos, output_size - out_pos, "\\u%04x",
                   (unsigned char)input[i]);
          out_pos += 6;
        }
      } else {
        output[out_pos++] = input[i];
      }
      break;
    }
  }
  output[out_pos] = '\0';
}

// JSON-RPC response helpers
static void send_response(const char *id, const char *result) {
  char buffer[4096];
  // Build the exact JSON-RPC response body (without trailing CRLF)
  int snprintf_result =
      snprintf(buffer, sizeof(buffer),
               "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);
  // Compute exact body length from snprintf return value (handles truncation)
  size_t body_len =
      (snprintf_result < 0 || (size_t)snprintf_result >= sizeof(buffer))
          ? sizeof(buffer) - 1
          : (size_t)snprintf_result;
  // Print header with exact length
  printf("Content-Length: %zu\r\n\r\n", body_len);
  // Write body payload separately with exact length
  fwrite(buffer, 1, body_len, stdout);
  fflush(stdout);
}

static void send_error(const char *id, int code, const char *message) {
  // First, JSON-escape the error message
  char escaped_message[512];
  json_escape(message ? message : "", escaped_message, sizeof(escaped_message));

  // Build the exact JSON-RPC error response body (without trailing CRLF)
  char body[1024];
  snprintf(body, sizeof(body),
           "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":"
           "\"%s\"}}",
           id, code, escaped_message);

  // Content-Length is the byte length of the body only (not including CRLF)
  size_t body_len = strlen(body);
  printf("Content-Length: %zu\r\n\r\n%s", body_len, body);
  fflush(stdout);
}

static void send_notification(const char *method, const char *params) {
  // First, compute required buffer size
  int required_size =
      snprintf(NULL, 0, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
               method, params);
  if (required_size < 0) {
    // Error in snprintf - cannot proceed
    return;
  }

  size_t body_len = (size_t)required_size;
  char stack_buffer[2048];
  char *buffer = stack_buffer;
  bool allocated = false;

  // Allocate heap buffer if stack buffer is too small
  if (body_len >= sizeof(stack_buffer)) {
    buffer = malloc(body_len + 1);
    if (!buffer) {
      // Allocation failed - cannot proceed
      return;
    }
    allocated = true;
  }

  // Build the JSON body
  int snprintf_result = snprintf(
      buffer, body_len + 1,
      "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  // Verify snprintf succeeded and produced expected length
  if (snprintf_result < 0 || (size_t)snprintf_result != body_len) {
    if (allocated) {
      free(buffer);
    }
    return;
  }

  // Print header with exact length
  printf("Content-Length: %zu\r\n\r\n", body_len);
  // Write body payload separately with exact length
  fwrite(buffer, 1, body_len, stdout);
  fflush(stdout);

  // Free heap buffer if allocated
  if (allocated) {
    free(buffer);
  }
}

// Diagnostic checking - parse file and return errors
static void check_diagnostics(const char *uri, const char *text) {
  // Tokenize
  TokenArray *tokens = tokenize(text, NULL);
  if (!tokens) {
    // Tokenization failed - build diagnostics JSON safely
    char diagnostics[4096];
    size_t pos = 0;
    size_t remaining = sizeof(diagnostics);

    int written = snprintf(diagnostics + pos, remaining,
                           "{\"uri\":\"%s\",\"diagnostics\":["
                           "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                           "\"end\":{\"line\":0,\"character\":1}},"
                           "\"severity\":1,"
                           "\"message\":\"Tokenization error\"}"
                           "]}",
                           uri);
    if (written < 0 || (size_t)written >= remaining) {
      // Buffer overflow or error - cannot send notification
      return;
    }
    send_notification("textDocument/publishDiagnostics", diagnostics);
    return;
  }

  // Parse
  AST *ast = parse(tokens);
  bool has_errors = (ast == NULL || ast->count == 0);

  // Build diagnostics JSON safely with position tracking
  char diagnostics[4096];
  size_t pos = 0;
  size_t remaining = sizeof(diagnostics);

  // Start building JSON
  int written = snprintf(diagnostics + pos, remaining,
                         "{\"uri\":\"%s\",\"diagnostics\":[", uri);
  if (written < 0 || (size_t)written >= remaining) {
    // Buffer overflow or error
    if (ast) {
      ast_free(ast);
    }
    token_array_free(tokens);
    return;
  }
  pos += (size_t)written;
  remaining -= (size_t)written;

  // Append diagnostic entry if there are errors
  if (has_errors && tokens->count > 0) {
    written = snprintf(diagnostics + pos, remaining,
                       "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                       "\"end\":{\"line\":0,\"character\":10}},"
                       "\"severity\":1,"
                       "\"message\":\"Syntax error: unexpected token\"}");
    if (written < 0 || (size_t)written >= remaining) {
      // Buffer overflow or error
      if (ast) {
        ast_free(ast);
      }
      token_array_free(tokens);
      return;
    }
    pos += (size_t)written;
    remaining -= (size_t)written;
  }

  // Close the JSON array and object
  written = snprintf(diagnostics + pos, remaining, "]}");
  if (written < 0 || (size_t)written >= remaining) {
    // Buffer overflow or error
    if (ast) {
      ast_free(ast);
    }
    token_array_free(tokens);
    return;
  }

  send_notification("textDocument/publishDiagnostics", diagnostics);

  // Cleanup
  if (ast) {
    ast_free(ast);
  }
  token_array_free(tokens);
}

// Handle LSP requests
static void handle_initialize(const char *id) {
  const char *capabilities =
      "{"
      "\"capabilities\":{"
      "\"textDocumentSync\":1,"
      "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]}"
      "}"
      "}";
  send_response(id, capabilities);
}

static void handle_shutdown(const char *id) { send_response(id, "null"); }

static void handle_did_open(const char *uri, const char *text) {
  check_diagnostics(uri, text);
}

static void handle_did_change(const char *uri, const char *text) {
  check_diagnostics(uri, text);
}

static void handle_completion(const char *id) {
  const char *completions =
      "{\"isIncomplete\":false,\"items\":["
      "{\"label\":\"set\",\"kind\":14,\"detail\":\"Immutable variable\"},"
      "{\"label\":\"let\",\"kind\":14,\"detail\":\"Mutable variable\"},"
      "{\"label\":\"if\",\"kind\":14,\"detail\":\"Conditional statement\"},"
      "{\"label\":\"for\",\"kind\":14,\"detail\":\"For loop\"},"
      "{\"label\":\"while\",\"kind\":14,\"detail\":\"While loop\"},"
      "{\"label\":\"and\",\"kind\":14,\"detail\":\"Logical AND operator\"},"
      "{\"label\":\"or\",\"kind\":14,\"detail\":\"Logical OR operator\"},"
      "{\"label\":\"not\",\"kind\":14,\"detail\":\"Logical NOT operator\"},"
      "{\"label\":\"list\",\"kind\":14,\"detail\":\"Create list literal\"},"
      "{\"label\":\"at\",\"kind\":14,\"detail\":\"List indexing operator\"},"
      "{\"label\":\"from\",\"kind\":14,\"detail\":\"List slicing operator\"},"
      "{\"label\":\"end\",\"kind\":14,\"detail\":\"End of list (for slicing)\"},"
      "{\"label\":\"len\",\"kind\":3,\"detail\":\"Get list length\"},"
      "{\"label\":\"function\",\"kind\":14,\"detail\":\"Define function\"},"
      "{\"label\":\"call\",\"kind\":14,\"detail\":\"Call function\"},"
      "{\"label\":\"return\",\"kind\":14,\"detail\":\"Return value\"},"
      "{\"label\":\"print\",\"kind\":14,\"detail\":\"Print value\"},"
      "{\"label\":\"true\",\"kind\":12,\"detail\":\"Boolean true\"},"
      "{\"label\":\"false\",\"kind\":12,\"detail\":\"Boolean false\"},"
      "{\"label\":\"null\",\"kind\":12,\"detail\":\"Null value\"},"
      "{\"label\":\"Pi\",\"kind\":21,\"detail\":\"Mathematical constant\"}"
      "]}";
  send_response(id, completions);
}

// Main LSP loop
int main(void) {
  fprintf(stderr, "Kronos LSP Server starting...\\n");

  char *body = NULL;
  size_t body_len = 0;

  while (read_lsp_message(&body, &body_len)) {
    if (!body)
      continue;

    char *method = json_get_string_value(body, "method");
    if (!method) {
      free(body);
      body = NULL;
      continue;
    }

    if (strcmp(method, "initialize") == 0) {
      char *id = json_get_id_value(body);
      handle_initialize(id ? id : "null");
      free(id);
    } else if (strcmp(method, "shutdown") == 0) {
      char *id = json_get_id_value(body);
      handle_shutdown(id ? id : "null");
      free(id);
      free(method);
      free(body);
      break;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
      char *uri = json_get_string_value(body, "uri");
      char *text = json_get_string_value(body, "text");
      if (uri && text) {
        handle_did_open(uri, text);
      } else {
        fprintf(stderr, "LSP didOpen missing uri/text\\n");
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
      char *uri = json_get_string_value(body, "uri");
      char *text = json_get_string_value(body, "text");
      if (uri && text) {
        handle_did_change(uri, text);
      } else {
        fprintf(stderr, "LSP didChange missing uri/text\\n");
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/completion") == 0) {
      char *id = json_get_id_value(body);
      handle_completion(id ? id : "null");
      free(id);
    } else {
      fprintf(stderr, "Unsupported LSP method: %s\\n", method);
    }

    free(method);
    free(body);
    body = NULL;
  }

  free(body);
  return 0;
}
