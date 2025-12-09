/**
 * @file lsp_messages.c
 * @brief JSON-RPC parsing and serialization for LSP server
 */

#include "lsp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * @brief Skip whitespace characters
 */
const char *skip_ws(const char *s) {
  while (s && *s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

/**
 * @brief Find the start of a JSON value for a given key
 */
const char *find_value_start(const char *json, const char *key) {
  if (!json || !key)
    return NULL;

  // Calculate required buffer size: key length + 2 quotes + null terminator
  size_t key_len = strlen(key);
  size_t pattern_size = key_len + 3; // +2 for quotes, +1 for null terminator

  // Use stack buffer for small keys (common case), dynamic allocation for large keys
  char stack_pattern[LSP_STACK_PATTERN_SIZE];
  char *pattern = stack_pattern;
  bool allocated = false;

  if (pattern_size > LSP_STACK_PATTERN_SIZE) {
    // Allocate dynamically for very long keys
    pattern = malloc(pattern_size);
    if (!pattern)
      return NULL; // Out of memory
    allocated = true;
  }

  int written = snprintf(pattern, pattern_size, "\"%s\"", key);
  if (written <= 0 || (size_t)written >= pattern_size) {
    if (allocated)
      free(pattern);
    return NULL; // snprintf error or truncation (shouldn't happen with correct size)
  }

  const char *pos = json;
  size_t pattern_len = (size_t)written;
  const char *result = NULL;

  while ((pos = strstr(pos, pattern)) != NULL) {
    pos += pattern_len;
    pos = skip_ws(pos);
    if (*pos != ':')
      continue;
    pos++;
    result = skip_ws(pos);
    break;
  }

  if (allocated)
    free(pattern);

  return result;
}

/**
 * @brief Extract a string value from JSON
 */
char *json_get_string_value(const char *json, const char *key) {
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

/**
 * @brief Extract an unquoted value from JSON
 */
char *json_get_unquoted_value(const char *json, const char *key) {
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

/**
 * @brief Extract the "id" field from a JSON-RPC message
 */
char *json_get_id_value(const char *json) {
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

/**
 * @brief Extract a nested JSON value using a dot-separated path
 */
char *json_get_nested_value(const char *json, const char *path) {
  if (!json || !path)
    return NULL;

  const char *current = json;
  char path_copy[LSP_PATTERN_BUFFER_SIZE];
  strncpy(path_copy, path, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  char *saveptr;
  char *token = strtok_r(path_copy, ".", &saveptr);
  while (token) {
    // Check if token is a number (array index)
    if (isdigit((unsigned char)token[0])) {
      // Array index - find the nth element
      int index = atoi(token);
      current = skip_ws(current);
      if (*current != '[')
        return NULL;
      current++; // skip '['
      current = skip_ws(current);

      // Find the index-th element
      int current_index = 0;
      while (current_index < index && *current != '\0' && *current != ']') {
        // Skip current element
        if (*current == '"') {
          // String value
          current++;
          while (*current != '\0' && *current != '"') {
            if (*current == '\\' && *(current + 1) != '\0')
              current += 2;
            else
              current++;
          }
          if (*current == '"')
            current++;
        } else if (*current == '{') {
          // Object - find matching '}'
          int depth = 1;
          current++;
          while (depth > 0 && *current != '\0') {
            if (*current == '{')
              depth++;
            else if (*current == '}')
              depth--;
            else if (*current == '"') {
              current++;
              while (*current != '\0' && *current != '"') {
                if (*current == '\\' && *(current + 1) != '\0')
                  current += 2;
                else
                  current++;
              }
            }
            if (*current != '\0')
              current++;
          }
        } else if (*current == '[') {
          // Array - find matching ']'
          int depth = 1;
          current++;
          while (depth > 0 && *current != '\0') {
            if (*current == '[')
              depth++;
            else if (*current == ']')
              depth--;
            else if (*current == '"') {
              current++;
              while (*current != '\0' && *current != '"') {
                if (*current == '\\' && *(current + 1) != '\0')
                  current += 2;
                else
                  current++;
              }
            }
            if (*current != '\0')
              current++;
          }
        } else {
          // Simple value - skip to comma or ]
          while (*current != '\0' && *current != ',' && *current != ']') {
            current++;
          }
        }
        current = skip_ws(current);
        if (*current == ',') {
          current++;
          current = skip_ws(current);
          current_index++;
        } else if (*current == ']') {
          break;
        }
      }
      if (current_index < index)
        return NULL;
      current = skip_ws(current);
    } else {
      // Object key
      current = find_value_start(current, token);
      if (!current)
        return NULL;
      current = skip_ws(current);
      if (*current == ':') {
        current++;
        current = skip_ws(current);
      }
    }
    token = strtok_r(NULL, ".", &saveptr);
  }

  if (!current)
    return NULL;

  // Extract the value at current position
  current = skip_ws(current);
  if (*current == '"') {
    // String value
    const char *value_start = current + 1;
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
  } else {
    // Unquoted value
    const char *cursor = current;
    while (*cursor && !isspace((unsigned char)*cursor) && *cursor != ',' &&
           *cursor != '}' && *cursor != ']') {
      cursor++;
    }
    size_t len = (size_t)(cursor - current);
    if (len == 0)
      return NULL;
    char *result = malloc(len + 1);
    if (!result)
      return NULL;
    memcpy(result, current, len);
    result[len] = '\0';
    return result;
  }
}

/**
 * @brief Read a JSON-RPC message from stdin
 */
bool read_lsp_message(char **out_body, size_t *out_length) {
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
      if (safe_strtoul(header_line + 15, &content_length)) {
        have_length = true;
      } else {
        // Invalid Content-Length header
        return false;
      }
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

/**
 * @brief Escape special characters in a string for JSON
 */
/**
 * @brief Escape a string for JSON, preserving newlines as \n for markdown
 * This is used for markdown content where newlines should be preserved
 */
void json_escape_markdown(const char *input, char *output, size_t output_size) {
  if (!input || !output || output_size == 0)
    return;

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
      // Preserve newlines as \n (not \\n) for markdown rendering
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
      if (out_pos < output_size - 1) {
        output[out_pos++] = input[i];
      }
      break;
    }
  }
  output[out_pos] = '\0';
}

void json_escape(const char *input, char *output, size_t output_size) {
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

/**
 * @brief Send a JSON-RPC response
 */
void send_response(const char *id, const char *result) {
  // Try with fixed-size buffer first (common case)
  char buffer[LSP_INITIAL_BUFFER_SIZE];
  int snprintf_result =
      snprintf(buffer, sizeof(buffer),
               "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result);

  size_t body_len;
  char *output = buffer;
  bool needs_free = false;

  if (snprintf_result < 0) {
    // Error in snprintf
    body_len = 0;
  } else if ((size_t)snprintf_result < sizeof(buffer)) {
    // Fits in buffer
    body_len = (size_t)snprintf_result;
  } else {
    // Needs larger buffer - allocate dynamically
    size_t needed = (size_t)snprintf_result + 1;
    output = malloc(needed);
    if (!output) {
      // Fallback: truncate
      body_len = sizeof(buffer) - 1;
      output = buffer;
    } else {
      needs_free = true;
      snprintf(output, needed, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
               id, result);
      body_len = needed - 1;
    }
  }

  printf("Content-Length: %zu\r\n\r\n", body_len);
  fwrite(output, 1, body_len, stdout);
  fflush(stdout);

  if (needs_free) {
    free(output);
  }
}

/**
 * @brief Send a JSON-RPC notification (no response expected)
 */
void send_notification(const char *method, const char *params) {
  int required_size =
      snprintf(NULL, 0, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
               method, params);
  if (required_size < 0)
    return;

  size_t body_len = (size_t)required_size;
  char stack_buffer[LSP_HOVER_BUFFER_SIZE];
  char *buffer = stack_buffer;
  bool allocated = false;

  if (body_len >= sizeof(stack_buffer)) {
    buffer = malloc(body_len + 1);
    if (!buffer)
      return;
    allocated = true;
  }

  int snprintf_result = snprintf(
      buffer, body_len + 1,
      "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}", method, params);

  if (snprintf_result < 0 || (size_t)snprintf_result != body_len) {
    if (allocated)
      free(buffer);
    return;
  }

  printf("Content-Length: %zu\r\n\r\n", body_len);
  fwrite(buffer, 1, body_len, stdout);
  fflush(stdout);

  if (allocated)
    free(buffer);
}
