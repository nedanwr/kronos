/**
 * @file lsp_main.c
 * @brief Main LSP server loop and dispatch
 */

#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Global document state (currently supports single file) */
DocumentState *g_doc = NULL;

/**
 * @brief Main LSP server loop
 *
 * Reads JSON-RPC messages from stdin and dispatches them to appropriate
 * handlers. Continues until shutdown request is received.
 *
 * Supported methods:
 * - initialize: Server initialization
 * - shutdown: Server shutdown
 * - textDocument/didOpen: Document opened
 * - textDocument/didChange: Document changed
 * - textDocument/completion: Code completion
 * - textDocument/definition: Go to definition
 * - textDocument/hover: Hover information
 * - textDocument/documentSymbol: Document outline
 * - textDocument/semanticTokens/full: Semantic highlighting
 * - workspace/symbol: Search symbols across workspace
 * - textDocument/codeLens: Show reference counts and parameter info
 *
 * @return 0 on normal exit
 */
int main(void) {
  fprintf(stderr, "Kronos LSP Server starting...\n");

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
      method = NULL;
      free(body);
      body = NULL;
      break;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
      // LSP spec: params.textDocument.uri and params.textDocument.text
      char *uri = json_get_nested_value(body, "params.textDocument.uri");
      if (!uri) {
        uri = json_get_string_value(body, "uri");
      }
      char *text = json_get_nested_value(body, "params.textDocument.text");
      if (!text) {
        text = json_get_string_value(body, "text");
      }
      if (uri && text) {
        handle_did_open(uri, text);
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
      // LSP spec: params.textDocument.uri and params.contentChanges[0].text
      char *uri = json_get_nested_value(body, "params.textDocument.uri");
      if (!uri) {
        uri = json_get_string_value(body, "uri");
      }
      // Get text from contentChanges[0].text
      char *text = json_get_nested_value(body, "params.contentChanges.0.text");
      if (!text) {
        // Fallback to params.text
        text = json_get_string_value(body, "text");
      }
      if (uri && text) {
        handle_did_change(uri, text);
      }
      free(uri);
      free(text);
    } else if (strcmp(method, "textDocument/completion") == 0) {
      char *id = json_get_id_value(body);
      handle_completion(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/definition") == 0) {
      char *id = json_get_id_value(body);
      handle_definition(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/references") == 0) {
      char *id = json_get_id_value(body);
      handle_references(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/prepareRename") == 0) {
      char *id = json_get_id_value(body);
      handle_prepare_rename(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/rename") == 0) {
      char *id = json_get_id_value(body);
      handle_rename(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/codeAction") == 0) {
      char *id = json_get_id_value(body);
      handle_code_action(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/formatting") == 0) {
      char *id = json_get_id_value(body);
      handle_formatting(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/hover") == 0) {
      char *id = json_get_id_value(body);
      handle_hover(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
      char *id = json_get_id_value(body);
      handle_document_symbols(id ? id : "null");
      free(id);
    } else if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
      char *id = json_get_id_value(body);
      handle_semantic_tokens(id ? id : "null");
      free(id);
    } else if (strcmp(method, "workspace/symbol") == 0) {
      char *id = json_get_id_value(body);
      handle_workspace_symbol(id ? id : "null", body);
      free(id);
    } else if (strcmp(method, "textDocument/codeLens") == 0) {
      char *id = json_get_id_value(body);
      handle_code_lens(id ? id : "null", body);
      free(id);
    } else {
      fprintf(stderr, "Unsupported LSP method: %s\n", method);
    }

    free(method);
    free(body);
    body = NULL;
  }

  free_document_state(g_doc);
  free(body);
  return 0;
}

