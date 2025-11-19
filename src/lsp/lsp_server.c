/*
 * Kronos Language Server Protocol Implementation (STUB)
 *
 * This is a minimal LSP server stub providing basic completion support.
 * It uses simplified string-based parsing and hardcoded values for prototyping.
 *
 * Current capabilities:
 * - Keyword completion (set, let, if, for, while, function, etc.)
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
      "\"completionProvider\":{\"triggerCharacters\":[\" \"]}"
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
  char buffer[8192];

  fprintf(stderr, "Kronos LSP Server starting...\n");

  while (fgets(buffer, sizeof(buffer), stdin)) {
    // NOTE: This is a STUB implementation using strstr() for quick prototyping.
    // Production LSP requires proper JSON-RPC parsing to extract:
    // - Request IDs from each message
    // - textDocument.uri and text from didOpen/didChange
    // - Position info from completion requests
    // TODO: Implement proper JSON parser or use library (e.g., cJSON)
    if (strstr(buffer, "initialize")) {
      handle_initialize("1");
    } else if (strstr(buffer, "shutdown")) {
      handle_shutdown("2");
      break;
    } else if (strstr(buffer, "textDocument/didOpen")) {
      // STUB: Uses hardcoded URI/text instead of parsing from request
      handle_did_open("file:///test.kr", "set x to 10");
    } else if (strstr(buffer, "textDocument/didChange")) {
      // STUB: Uses hardcoded URI/text instead of parsing from request
      handle_did_change("file:///test.kr", "set x to 10");
    } else if (strstr(buffer, "textDocument/completion")) {
      // STUB: Uses hardcoded ID instead of parsing from request
      handle_completion("3");
    }
  }

  return 0;
}
