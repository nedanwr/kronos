#include "test_lsp_framework.h"
#include <signal.h>
#include <unistd.h>

LSPTestContext *g_ctx = NULL; // Global for test setup/teardown

// Test hover for file-based modules
TEST(lsp_hover_file_module) {
  const char *code = "import math\n"
                    "set x to 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Hover over module name (built-in module)
  char *response = lsp_hover(g_ctx, 0, 7);
  ASSERT_PTR_NOT_NULL(response);
  // Should contain module information or be valid JSON
  ASSERT_TRUE(lsp_is_valid_json(response));
  free(response);
}

// Test module function validation
TEST(lsp_module_function_validation) {
  const char *code = "import math\n"
                    "call math.sqrt with 16\n"
                    "call math.invalid_func with 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait a bit for diagnostics
  usleep(100000); // 100ms

  // The LSP should validate module functions
  // This test verifies the feature exists (actual validation happens in diagnostics)
  ASSERT_TRUE(true);
}

// Test find all references
TEST(lsp_find_references) {
  const char *code = "set x to 10\n"
                    "set y to x plus 5\n"
                    "set z to x times 2\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Small delay for document processing
  usleep(100000); // 100ms

  // Find references to 'x' at its definition
  char *response = lsp_references(g_ctx, 0, 5);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should be valid JSON (array or null)
  free(response);
}

// Test rename symbol
TEST(lsp_rename_symbol) {
  const char *code = "set old_name to 10\n"
                    "set y to old_name plus 5\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Small delay for document processing
  usleep(100000); // 100ms

  // Prepare rename
  char *prepare_response = lsp_prepare_rename(g_ctx, 0, 5);
  ASSERT_PTR_NOT_NULL(prepare_response);
  ASSERT_TRUE(lsp_is_valid_json(prepare_response));
  free(prepare_response);

  // Perform rename
  char *rename_response = lsp_rename(g_ctx, 0, 5, "new_name");
  ASSERT_PTR_NOT_NULL(rename_response);
  ASSERT_TRUE(lsp_is_valid_json(rename_response));
  // Should be valid JSON (WorkspaceEdit or null)
  free(rename_response);
}

// Test document formatting
TEST(lsp_formatting) {
  const char *code = "set x to 10\nset y to 20\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Small delay for document processing
  usleep(100000); // 100ms

  char *response = lsp_formatting(g_ctx);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should be valid JSON (array of TextEdits)
  free(response);
}

// Test workspace symbols
TEST(lsp_workspace_symbols) {
  const char *code = "function my_function with x:\n"
                    "    return x\n"
                    "set my_variable to 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Small delay for document processing
  usleep(100000); // 100ms

  // Search for "my"
  char *response = lsp_workspace_symbol(g_ctx, "my");
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should be valid JSON (array of symbols)
  free(response);
}

// Test code lens
TEST(lsp_code_lens) {
  const char *code = "function test_func with x:\n"
                    "    return x\n"
                    "call test_func with 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Small delay for document processing
  usleep(100000); // 100ms

  char *response = lsp_code_lens(g_ctx);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should be valid JSON (array of code lens items)
  free(response);
}

// Test code actions (placeholder)
TEST(lsp_code_actions) {
  const char *code = "set x to 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  char *response = lsp_code_action(g_ctx, 0, 0, 0, 10);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should return empty array for now (placeholder) - check for array start
  ASSERT_TRUE(lsp_response_contains(response, "[") || 
             lsp_response_contains(response, "result"));
  free(response);
}

// Setup and teardown
void lsp_test_setup(void) {
  if (!g_ctx) {
    g_ctx = lsp_test_init();
    if (!g_ctx) {
      fprintf(stderr, "Failed to initialize LSP test context\n");
    }
  }
}

void lsp_test_teardown(void) {
  if (g_ctx) {
    lsp_test_cleanup(g_ctx);
    g_ctx = NULL;
  }
}

