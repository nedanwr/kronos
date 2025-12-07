#ifndef TEST_LSP_FRAMEWORK_H
#define TEST_LSP_FRAMEWORK_H

#include "../framework/test_framework.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// LSP test context
typedef struct {
  FILE *lsp_stdin;   // Write to LSP server
  FILE *lsp_stdout;  // Read from LSP server
  FILE *lsp_stderr;  // Read stderr from LSP server
  int lsp_pid;       // Process ID of LSP server
} LSPTestContext;

// Initialize LSP test context (starts LSP server)
LSPTestContext *lsp_test_init(void);

// Cleanup LSP test context (stops LSP server)
void lsp_test_cleanup(LSPTestContext *ctx);

// Send a JSON-RPC request to the LSP server
bool lsp_send_request(LSPTestContext *ctx, const char *method, const char *params,
                     int id);

// Read response from LSP server (with timeout)
char *lsp_read_response(LSPTestContext *ctx, int timeout_ms);

// Send initialize request
bool lsp_initialize(LSPTestContext *ctx);

// Send didOpen request
bool lsp_did_open(LSPTestContext *ctx, const char *uri, const char *text);

// Send didChange request
bool lsp_did_change(LSPTestContext *ctx, const char *uri, const char *text);

// Send hover request
char *lsp_hover(LSPTestContext *ctx, int line, int character);

// Send references request
char *lsp_references(LSPTestContext *ctx, int line, int character);

// Send prepareRename request
char *lsp_prepare_rename(LSPTestContext *ctx, int line, int character);

// Send rename request
char *lsp_rename(LSPTestContext *ctx, int line, int character, const char *new_name);

// Send codeAction request
char *lsp_code_action(LSPTestContext *ctx, int start_line, int start_char,
                     int end_line, int end_char);

// Send formatting request
char *lsp_formatting(LSPTestContext *ctx);

// Send workspace/symbol request
char *lsp_workspace_symbol(LSPTestContext *ctx, const char *query);

// Send codeLens request
char *lsp_code_lens(LSPTestContext *ctx);

// Helper to extract JSON value from response
char *lsp_extract_json_value(const char *json, const char *key);

// Helper to check if response contains a string
bool lsp_response_contains(const char *response, const char *substring);

// Helper to check if response is valid JSON
bool lsp_is_valid_json(const char *response);

#endif // TEST_LSP_FRAMEWORK_H

