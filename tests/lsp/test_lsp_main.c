#include "test_lsp_framework.h"
#include "../framework/test_framework.h"
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

extern void test_run_all(void);
extern void lsp_test_setup(void);
extern void lsp_test_teardown(void);

// Global LSP context for tests
extern LSPTestContext *g_ctx;

int main(void) {
  // Check if LSP server exists
  if (access("./kronos-lsp", F_OK) != 0) {
    fprintf(stderr, "Error: kronos-lsp not found. Run 'make lsp' first.\n");
    return 1;
  }

  test_framework_init();

  // Setup LSP test context before running tests
  lsp_test_setup();
  
  // Check if setup succeeded
  if (!g_ctx) {
    fprintf(stderr, "Error: Failed to initialize LSP test context\n");
    fprintf(stderr, "Make sure kronos-lsp is built (run 'make lsp')\n");
    test_framework_cleanup();
    return 1;
  }

  // Small delay to let LSP server fully initialize
  usleep(50000); // 50ms

  // Run all registered tests
  test_run_all();

  // Teardown LSP test context
  lsp_test_teardown();

  // Print results
  test_print_results();

  // Get exit code before cleanup
  int exit_code = test_get_exit_code();

  // Cleanup
  test_framework_cleanup();

  return exit_code;
}

