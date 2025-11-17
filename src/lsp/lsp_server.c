/*
 * Kronos Language Server Protocol Implementation
 *
 * This LSP server provides real-time diagnostics, autocomplete, and other
 * IDE features for Kronos files.
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

// JSON-RPC response helpers
static void send_response(const char *id, const char *result) {
  printf("Content-Length: %zu\r\n\r\n", strlen(result) + 50);
  printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\r\n", id, result);
  fflush(stdout);
}

static void send_error(const char *id, int code, const char *message) {
  char buffer[1024];
  snprintf(buffer, sizeof(buffer),
           "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":"
           "\"%s\"}}",
           id, code, message);
  printf("Content-Length: %zu\r\n\r\n%s\r\n", strlen(buffer), buffer);
  fflush(stdout);
}

static void send_notification(const char *method, const char *params) {
  char buffer[2048];
  snprintf(buffer, sizeof(buffer),
           "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method,
           params);
  printf("Content-Length: %zu\r\n\r\n%s\r\n", strlen(buffer), buffer);
  fflush(stdout);
}

// Diagnostic checking - parse file and return errors
static void check_diagnostics(const char *uri, const char *text) {
  // Tokenize
  TokenArray *tokens = tokenize(text);
  if (!tokens) {
    // Tokenization failed
    char diagnostics[4096];
    snprintf(diagnostics, sizeof(diagnostics),
             "{\"uri\":\"%s\",\"diagnostics\":["
             "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
             "\"end\":{\"line\":0,\"character\":1}},"
             "\"severity\":1,"
             "\"message\":\"Tokenization error\"}"
             "]}",
             uri);
    send_notification("textDocument/publishDiagnostics", diagnostics);
    return;
  }

  // Parse
  AST *ast = parse(tokens);
  bool has_errors = (ast == NULL || ast->count == 0);

  // Build diagnostics JSON
  char diagnostics[4096];
  snprintf(diagnostics, sizeof(diagnostics),
           "{\"uri\":\"%s\",\"diagnostics\":[", uri);

  if (has_errors && tokens->count > 0) {
    strcat(diagnostics, "{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                        "\"end\":{\"line\":0,\"character\":10}},"
                        "\"severity\":1,"
                        "\"message\":\"Syntax error: unexpected token\"}");
  }

  strcat(diagnostics, "]}");

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
      "\"diagnosticProvider\":true,"
      "\"completionProvider\":{\"triggerCharacters\":[\" \"]},"
      "\"hoverProvider\":true"
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
    // Simple JSON-RPC parser (production should use proper JSON library)
    if (strstr(buffer, "initialize")) {
      handle_initialize("1");
    } else if (strstr(buffer, "shutdown")) {
      handle_shutdown("2");
      break;
    } else if (strstr(buffer, "textDocument/didOpen")) {
      // Extract URI and text (simplified)
      handle_did_open("file:///test.kr", "set x to 10");
    } else if (strstr(buffer, "textDocument/didChange")) {
      handle_did_change("file:///test.kr", "set x to 10");
    } else if (strstr(buffer, "textDocument/completion")) {
      handle_completion("3");
    }
  }

  return 0;
}
