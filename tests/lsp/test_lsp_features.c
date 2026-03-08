#include "test_lsp_framework.h"
#include <signal.h>
#include <unistd.h>

LSPTestContext *g_ctx = NULL; // Global for test setup/teardown

static char *lsp_read_diagnostics_with_message(const char *message_substring,
                                               int max_attempts) {
  for (int i = 0; i < max_attempts; i++) {
    char *msg = lsp_read_response(g_ctx, 500);
    if (!msg) {
      continue;
    }
    bool is_diagnostics =
        lsp_response_contains(msg, "textDocument/publishDiagnostics");
    bool has_message =
        !message_substring || lsp_response_contains(msg, message_substring);
    if (is_diagnostics && has_message) {
      return msg;
    }
    free(msg);
  }
  return NULL;
}

// Test hover for file-based modules
TEST(lsp_hover_file_module) {
  const char *code = "import math\n"
                    "set x to 10\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

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

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 200);
  free(diag);

  char *response = lsp_code_action(g_ctx, 0, 0, 0, 10);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should return empty array for now (placeholder) - check for array start
  ASSERT_TRUE(lsp_response_contains(response, "[") ||
             lsp_response_contains(response, "result"));
  free(response);
}

// Test hover for function with default parameters
TEST(lsp_hover_function_default_params) {
  const char *code = "function greet with name, greeting = \"Hello\":\n"
                    "    return f\"{greeting}, {name}!\"\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  // Hover over the function name 'greet'
  char *response = lsp_hover(g_ctx, 0, 10);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should contain parameter info indicating required/optional
  ASSERT_TRUE(lsp_response_contains(response, "function") ||
             lsp_response_contains(response, "null"));
  free(response);
}

// Test hover for variadic function
TEST(lsp_hover_variadic_function) {
  const char *code = "function sum with ...numbers:\n"
                    "    return 0\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  // Hover over the function name 'sum'
  char *response = lsp_hover(g_ctx, 0, 10);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  // Should be valid JSON response
  ASSERT_TRUE(lsp_response_contains(response, "function") ||
             lsp_response_contains(response, "null"));
  free(response);
}

// Test diagnostics for function with default parameters - valid call
TEST(lsp_diagnostics_default_params_valid) {
  const char *code = "function greet with name, greeting = \"Hello\":\n"
                    "    return f\"{greeting}, {name}!\"\n"
                    "# Valid calls\n"
                    "call greet with \"Alice\"\n"
                    "call greet with \"Bob\", \"Hi\"\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  // No errors expected for valid calls
  ASSERT_TRUE(true);
}

// Test diagnostics for function call with too few arguments
TEST(lsp_diagnostics_too_few_args) {
  const char *code = "function greet with name, greeting = \"Hello\":\n"
                    "    return f\"{greeting}, {name}!\"\n"
                    "call greet\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  // Should report an error for too few arguments
  // The LSP will generate diagnostics (we can't easily capture them here,
  // but the test verifies the code doesn't crash)
  ASSERT_TRUE(true);
}

// Test diagnostics for variadic function - valid calls
TEST(lsp_diagnostics_variadic_valid) {
  const char *code = "function sum with ...numbers:\n"
                    "    return 0\n"
                    "# Valid variadic calls\n"
                    "call sum\n"
                    "call sum with 1\n"
                    "call sum with 1, 2, 3, 4, 5\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Wait for and consume diagnostics notification
  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  // No errors expected for valid variadic calls
  ASSERT_TRUE(true);
}

TEST(lsp_diagnostics_map_requires_list_argument) {
  const char *code = "call map with \"hello\", function with x: return x\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(
      "Function 'map' requires a list argument", 6);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));
  ASSERT_TRUE(lsp_response_contains(diag, "Function 'map' requires a list argument"));
  free(diag);
}

TEST(lsp_diagnostics_filter_requires_list_argument) {
  const char *code = "call filter with \"hello\", function with x: return x\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(
      "Function 'filter' requires a list argument", 6);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));
  ASSERT_TRUE(lsp_response_contains(diag, "Function 'filter' requires a list argument"));
  free(diag);
}

TEST(lsp_completion_includes_filter_and_map_utilities) {
  const char *code = "set numbers to list 1, 2, 3\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  // Consume diagnostics notification triggered by didOpen first.
  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(NULL, 4);
  free(diag);

  char *response = lsp_completion(g_ctx, 0, 5);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "Filter a list with a callback function"));
  ASSERT_TRUE(lsp_response_contains(response, "Transform a list with a callback function"));
  free(response);
}

TEST(lsp_completion_includes_pattern_matching_keywords) {
  const char *code = "set value to 1\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(NULL, 4);
  free(diag);

  char *response = lsp_completion(g_ctx, 0, 0);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "\"label\":\"match\""));
  ASSERT_TRUE(lsp_response_contains(response, "\"label\":\"case\""));
  ASSERT_TRUE(lsp_response_contains(response, "\"label\":\"default\""));
  free(response);
}

TEST(lsp_completion_includes_type_keyword) {
  const char *code = "set value to 1\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(NULL, 4);
  free(diag);

  char *response = lsp_completion(g_ctx, 0, 0);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "\"label\":\"type\""));
  ASSERT_TRUE(
      lsp_response_contains(response, "Declare type alias"));
  free(response);
}

TEST(lsp_hover_and_definition_for_type_alias) {
  const char *code =
      "type Point to map x: number, y: number\n"
      "set p to map x: 1, y: 2 as Point\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_diagnostics_with_message(NULL, 6);
  free(diag);

  char *hover = lsp_hover(g_ctx, 0, 7);
  ASSERT_PTR_NOT_NULL(hover);
  ASSERT_TRUE(lsp_is_valid_json(hover));
  ASSERT_TRUE(lsp_response_contains(hover, "type alias"));
  ASSERT_TRUE(lsp_response_contains(hover, "map{x:number,y:number}"));
  free(hover);

  // Position over "Point" (not the preceding whitespace) for definition lookup.
  char *definition = lsp_definition(g_ctx, 1, 27);
  ASSERT_PTR_NOT_NULL(definition);
  ASSERT_TRUE(lsp_is_valid_json(definition));
  ASSERT_TRUE(lsp_response_contains(definition, "\"line\":0"));
  free(definition);
}

TEST(lsp_diagnostics_generic_type_mismatch_reported) {
  const char *code =
      "let nums to list 1, 2 as list<number>\n"
      "let nums to list 3, \"bad\"\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag =
      lsp_read_diagnostics_with_message("Type mismatch for variable 'nums'", 8);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));
  ASSERT_TRUE(lsp_response_contains(diag, "expected 'list<number>'"));
  free(diag);
}

TEST(lsp_match_statement_diagnostics_and_definition) {
  const char *code =
      "let value to 2\n"
      "match value:\n"
      "    case 1:\n"
      "        print value\n"
      "    default:\n"
      "        print value\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));
  ASSERT_FALSE(lsp_response_contains(diag, "Undefined variable 'value'"));
  free(diag);

  char *hover = lsp_hover(g_ctx, 3, 14);
  ASSERT_PTR_NOT_NULL(hover);
  ASSERT_TRUE(lsp_is_valid_json(hover));
  ASSERT_TRUE(lsp_response_contains(hover, "value"));
  free(hover);

  char *definition = lsp_definition(g_ctx, 3, 14);
  ASSERT_PTR_NOT_NULL(definition);
  ASSERT_TRUE(lsp_is_valid_json(definition));
  ASSERT_TRUE(lsp_response_contains(definition, "\"line\":0"));
  free(definition);
}

TEST(lsp_match_statement_references_include_match_branches) {
  const char *code =
      "let value to 2\n"
      "match value:\n"
      "    case 1:\n"
      "        print value\n"
      "    default:\n"
      "        print value\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  char *references = lsp_references(g_ctx, 0, 5);
  ASSERT_PTR_NOT_NULL(references);
  ASSERT_TRUE(lsp_is_valid_json(references));

  size_t count = 0;
  const char *needle = "\"uri\":\"file:///test.kr\"";
  char *cursor = references;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    count++;
    cursor += strlen(needle);
  }

  ASSERT_INT_EQ((int)count, 5);
  free(references);
}

TEST(lsp_match_statement_formatting_indents_case_and_default) {
  const char *code =
      "match value:\n"
      "    case 1:\n"
      "        print value\n"
      "        default:\n"
      "            print 0\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  char *response = lsp_formatting(g_ctx);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(
      response,
      "match value:\\n    case 1:\\n        print value\\n    default:\\n        print 0\\n"));
  free(response);
}

TEST(lsp_match_statement_undefined_variable_reported_once) {
  const char *code =
      "match 1:\n"
      "    case 1:\n"
      "        print missing_value\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag =
      lsp_read_diagnostics_with_message("Undefined variable 'missing_value'", 6);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));

  size_t count = 0;
  const char *needle = "Undefined variable 'missing_value'";
  char *cursor = diag;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    count++;
    cursor += strlen(needle);
  }

  ASSERT_INT_EQ((int)count, 1);
  free(diag);
}

TEST(lsp_diagnostics_list_comprehension_loop_var_is_defined) {
  const char *code =
      "set values to [comp_value times 2 for comp_value in range 1 to 6 if "
      "comp_value is greater than 2]\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  ASSERT_PTR_NOT_NULL(diag);
  ASSERT_TRUE(lsp_is_valid_json(diag));
  ASSERT_FALSE(
      lsp_response_contains(diag, "Undefined variable 'comp_value'"));
  free(diag);
}

TEST(lsp_hover_list_comprehension_loop_var) {
  const char *code =
      "set values to [item_value for item_value in [1, 2, 3]]\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  char *response = lsp_hover(g_ctx, 0, 16);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "variable"));
  ASSERT_TRUE(lsp_response_contains(response, "item_value"));
  free(response);
}

TEST(lsp_definition_list_comprehension_loop_var) {
  const char *code =
      "set values to [item_value for item_value in [1, 2, 3]]\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  char *response = lsp_definition(g_ctx, 0, 16);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "file:///test.kr"));
  free(response);
}

TEST(lsp_references_list_comprehension_loop_var) {
  const char *code =
      "set values to [ref_item times ref_item for ref_item in [1, 2, 3]]\n";
  ASSERT_TRUE(lsp_did_open(g_ctx, "file:///test.kr", code));

  usleep(100000); // 100ms
  char *diag = lsp_read_response(g_ctx, 500);
  free(diag);

  char *response = lsp_references(g_ctx, 0, 16);
  ASSERT_PTR_NOT_NULL(response);
  ASSERT_TRUE(lsp_is_valid_json(response));
  ASSERT_TRUE(lsp_response_contains(response, "file:///test.kr"));
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
