#include "test_lsp_framework.h"
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

// Helper to create JSON-RPC message
static char *create_jsonrpc_message(const char *method, const char *params,
                                   int id) {
  // First, build the JSON body to calculate exact length
  char json_body[4096];
  size_t body_len;
  
  if (params) {
    body_len = (size_t)snprintf(json_body, sizeof(json_body),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}",
            id, method, params);
  } else {
    body_len = (size_t)snprintf(json_body, sizeof(json_body),
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
            id, method);
  }

  // Now build the full message with correct Content-Length
  char *message = malloc(1024 + body_len);
  if (!message)
    return NULL;

  snprintf(message, 1024 + body_len,
          "Content-Length: %zu\r\n\r\n%s",
          body_len, json_body);

  return message;
}

// Helper to read Content-Length header
static int read_content_length(FILE *fp) {
  char line[256];
  if (!fgets(line, sizeof(line), fp))
    return -1;

  // Look for "Content-Length: "
  const char *prefix = "Content-Length: ";
  if (strncmp(line, prefix, strlen(prefix)) != 0)
    return -1;

  int length = atoi(line + strlen(prefix));
  // Read blank line
  fgets(line, sizeof(line), fp);
  return length;
}

LSPTestContext *lsp_test_init(void) {
  LSPTestContext *ctx = calloc(1, sizeof(LSPTestContext));
  if (!ctx)
    return NULL;

  // Create pipes for communication
  int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
    free(ctx);
    return NULL;
  }

  // Fork to start LSP server
  ctx->lsp_pid = fork();
  if (ctx->lsp_pid == 0) {
    // Child process: LSP server
    close(stdin_pipe[1]);  // Close write end
    close(stdout_pipe[0]); // Close read end
    close(stderr_pipe[0]); // Close read end

    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // Execute LSP server
    execl("./kronos-lsp", "kronos-lsp", (char *)NULL);
    exit(1);
  } else if (ctx->lsp_pid > 0) {
    // Parent process: test runner
    close(stdin_pipe[0]);  // Close read end
    close(stdout_pipe[1]); // Close write end
    close(stderr_pipe[1]); // Close write end

    ctx->lsp_stdin = fdopen(stdin_pipe[1], "w");
    ctx->lsp_stdout = fdopen(stdout_pipe[0], "r");
    ctx->lsp_stderr = fdopen(stderr_pipe[0], "r");

    if (!ctx->lsp_stdin || !ctx->lsp_stdout) {
      lsp_test_cleanup(ctx);
      return NULL;
    }

    // Set line buffering for stdin, full buffering for stdout
    setvbuf(ctx->lsp_stdin, NULL, _IOLBF, 0);
    setvbuf(ctx->lsp_stdout, NULL, _IOFBF, 8192);

    // Initialize the LSP server
    if (!lsp_initialize(ctx)) {
      lsp_test_cleanup(ctx);
      return NULL;
    }

    return ctx;
  } else {
    // Fork failed
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    free(ctx);
    return NULL;
  }
}

void lsp_test_cleanup(LSPTestContext *ctx) {
  if (!ctx)
    return;

  if (ctx->lsp_stdin)
    fclose(ctx->lsp_stdin);
  if (ctx->lsp_stdout)
    fclose(ctx->lsp_stdout);
  if (ctx->lsp_stderr)
    fclose(ctx->lsp_stderr);

  if (ctx->lsp_pid > 0) {
    kill(ctx->lsp_pid, SIGTERM);
    waitpid(ctx->lsp_pid, NULL, 0);
  }

  free(ctx);
}

bool lsp_send_request(LSPTestContext *ctx, const char *method, const char *params,
                     int id) {
  if (!ctx || !ctx->lsp_stdin)
    return false;

  char *message = create_jsonrpc_message(method, params, id);
  if (!message)
    return false;

  size_t len = strlen(message);
  size_t written = fwrite(message, 1, len, ctx->lsp_stdin);
  fflush(ctx->lsp_stdin);

  free(message);
  return written == len;
}

char *lsp_read_response(LSPTestContext *ctx, int timeout_ms) {
  if (!ctx || !ctx->lsp_stdout)
    return NULL;

  int length = read_content_length(ctx->lsp_stdout);
  if (length <= 0 || length > 100000) // Sanity check
    return NULL;

  char *response = malloc(length + 1);
  if (!response)
    return NULL;

  size_t read = fread(response, 1, length, ctx->lsp_stdout);
  response[read] = '\0';

  return response;
}

bool lsp_initialize(LSPTestContext *ctx) {
  const char *params = "{\"capabilities\":{},\"rootUri\":null}";
  if (!lsp_send_request(ctx, "initialize", params, 1))
    return false;

  char *response = lsp_read_response(ctx, 1000);
  if (!response)
    return false;

  bool success = strstr(response, "\"result\"") != NULL;
  free(response);
  return success;
}

// Simple JSON string escape
static void json_escape_string(const char *input, char *output, size_t output_size) {
  size_t out_pos = 0;
  for (size_t i = 0; input[i] != '\0' && out_pos < output_size - 1; i++) {
    switch (input[i]) {
    case '"':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '"';
      }
      break;
    case '\\':
      if (out_pos < output_size - 2) {
        output[out_pos++] = '\\';
        output[out_pos++] = '\\';
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
      output[out_pos++] = input[i];
      break;
    }
  }
  output[out_pos] = '\0';
}

bool lsp_did_open(LSPTestContext *ctx, const char *uri, const char *text) {
  char params[8192];
  char escaped_uri[1024];
  char escaped_text[4096];

  // JSON escape strings
  json_escape_string(uri, escaped_uri, sizeof(escaped_uri));
  json_escape_string(text, escaped_text, sizeof(escaped_text));

  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"%s\",\"text\":\"%s\"}}", escaped_uri,
          escaped_text);

  return lsp_send_request(ctx, "textDocument/didOpen", params, 0);
}

bool lsp_did_change(LSPTestContext *ctx, const char *uri, const char *text) {
  char params[8192];
  char escaped_uri[1024];
  char escaped_text[4096];

  // JSON escape strings
  json_escape_string(uri, escaped_uri, sizeof(escaped_uri));
  json_escape_string(text, escaped_text, sizeof(escaped_text));

  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"%s\"},\"contentChanges\":[{\"text\":\"%s\"}]}",
          escaped_uri, escaped_text);

  return lsp_send_request(ctx, "textDocument/didChange", params, 0);
}

char *lsp_hover(LSPTestContext *ctx, int line, int character) {
  char params[256];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"},"
          "\"position\":{\"line\":%d,\"character\":%d}}",
          line, character);

  if (!lsp_send_request(ctx, "textDocument/hover", params, 2))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_references(LSPTestContext *ctx, int line, int character) {
  char params[256];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"},"
          "\"position\":{\"line\":%d,\"character\":%d}}",
          line, character);

  if (!lsp_send_request(ctx, "textDocument/references", params, 3))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_prepare_rename(LSPTestContext *ctx, int line, int character) {
  char params[256];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"},"
          "\"position\":{\"line\":%d,\"character\":%d}}",
          line, character);

  if (!lsp_send_request(ctx, "textDocument/prepareRename", params, 4))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_rename(LSPTestContext *ctx, int line, int character,
                const char *new_name) {
  char params[512];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"},"
          "\"position\":{\"line\":%d,\"character\":%d},"
          "\"newName\":\"%s\"}",
          line, character, new_name);

  if (!lsp_send_request(ctx, "textDocument/rename", params, 5))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_code_action(LSPTestContext *ctx, int start_line, int start_char,
                     int end_line, int end_char) {
  char params[512];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"},"
          "\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
          "\"end\":{\"line\":%d,\"character\":%d}}}",
          start_line, start_char, end_line, end_char);

  if (!lsp_send_request(ctx, "textDocument/codeAction", params, 6))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_formatting(LSPTestContext *ctx) {
  char params[256];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"}}");

  if (!lsp_send_request(ctx, "textDocument/formatting", params, 7))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_workspace_symbol(LSPTestContext *ctx, const char *query) {
  char params[512];
  snprintf(params, sizeof(params), "{\"query\":\"%s\"}", query);

  if (!lsp_send_request(ctx, "workspace/symbol", params, 8))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_code_lens(LSPTestContext *ctx) {
  char params[256];
  snprintf(params, sizeof(params),
          "{\"textDocument\":{\"uri\":\"file:///test.kr\"}}");

  if (!lsp_send_request(ctx, "textDocument/codeLens", params, 9))
    return NULL;

  return lsp_read_response(ctx, 1000);
}

char *lsp_extract_json_value(const char *json, const char *key) {
  // Simple JSON value extraction (for testing)
  char pattern[256];
  snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char *pos = strstr(json, pattern);
  if (!pos)
    return NULL;

  pos += strlen(pattern);
  while (*pos == ' ' || *pos == '\t')
    pos++;

  if (*pos == '"') {
    // String value
    pos++;
    const char *end = strchr(pos, '"');
    if (!end)
      return NULL;
    size_t len = end - pos;
    char *value = malloc(len + 1);
    if (value) {
      memcpy(value, pos, len);
      value[len] = '\0';
    }
    return value;
  } else if (*pos == '{' || *pos == '[') {
    // Object or array - find matching bracket
    int depth = 1;
    const char *start = pos;
    pos++;
    while (*pos && depth > 0) {
      if (*pos == '{' || *pos == '[')
        depth++;
      else if (*pos == '}' || *pos == ']')
        depth--;
      pos++;
    }
    size_t len = pos - start;
    char *value = malloc(len + 1);
    if (value) {
      memcpy(value, start, len);
      value[len] = '\0';
    }
    return value;
  }

  return NULL;
}

bool lsp_response_contains(const char *response, const char *substring) {
  if (!response || !substring)
    return false;
  return strstr(response, substring) != NULL;
}

bool lsp_is_valid_json(const char *response) {
  if (!response)
    return false;
  // Basic check: starts with { or [
  return response[0] == '{' || response[0] == '[' || response[0] == 'n'; // null
}

