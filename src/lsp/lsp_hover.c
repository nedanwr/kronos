/**
 * @file lsp_hover.c
 * @brief Hover information for LSP server
 */

#include "lsp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

#define LSP_HOVER_INITIAL_CAPACITY 256
#define LSP_HOVER_MAX_MARKDOWN_SIZE (64 * 1024)
#define LSP_HOVER_MAX_JSON_SIZE (128 * 1024)

static const char *method_description(const char *name) {
  static const struct {
    const char *name;
    const char *description;
  } methods[] = {
      {"uppercase", "Convert the receiver string to uppercase."},
      {"lowercase", "Convert the receiver string to lowercase."},
      {"trim", "Remove leading and trailing whitespace from the receiver."},
      {"split", "Split the receiver string using a delimiter."},
      {"capitalize", "Uppercase the first character of the receiver string."},
      {"title", "Capitalize each word in the receiver string."},
      {"filter", "Keep receiver-list items accepted by a callback."},
      {"map", "Transform each receiver-list item with a callback."},
      {"reverse", "Reverse the receiver list."},
      {"sort", "Sort the receiver list."},
      {"len", "Return the length of the receiver."},
  };
  for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
    if (strcmp(methods[i].name, name) == 0) {
      return methods[i].description;
    }
  }
  return NULL;
}

static bool hover_appendf(char **buffer, size_t *length, size_t *capacity,
                          size_t max_length, const char *fmt, ...) {
  if (!buffer || !length || !capacity || !fmt) {
    return false;
  }

  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    return false;
  }

  size_t append_len = (size_t)needed;
  if (*length > max_length || append_len > max_length - *length) {
    return false;
  }

  size_t required_size = *length + append_len + 1;
  if (*capacity < required_size) {
    size_t new_capacity =
        *capacity > 0 ? *capacity : LSP_HOVER_INITIAL_CAPACITY;
    size_t max_capacity = max_length + 1;

    while (new_capacity < required_size) {
      if (new_capacity >= max_capacity / 2) {
        new_capacity = max_capacity;
      } else {
        new_capacity *= 2;
      }
      if (new_capacity == max_capacity) {
        break;
      }
    }

    if (new_capacity < required_size || new_capacity > max_capacity) {
      return false;
    }

    char *grown = realloc(*buffer, new_capacity);
    if (!grown) {
      return false;
    }
    *buffer = grown;
    *capacity = new_capacity;
  }

  va_start(args, fmt);
  int written =
      vsnprintf(*buffer + *length, *capacity - *length, fmt, args);
  va_end(args);
  if (written < 0 || (size_t)written != append_len) {
    return false;
  }

  *length += append_len;
  return true;
}

static bool send_markdown_hover_response(const char *id,
                                         const char *markdown_text) {
  if (!id || !markdown_text) {
    return false;
  }

  size_t escaped_len = json_escape_markdown(markdown_text, NULL, 0);
  if (escaped_len > LSP_HOVER_MAX_JSON_SIZE) {
    return false;
  }

  char *escaped_hover = malloc(escaped_len + 1);
  if (!escaped_hover) {
    return false;
  }

  size_t escaped_written =
      json_escape_markdown(markdown_text, escaped_hover, escaped_len + 1);
  if (escaped_written != escaped_len) {
    free(escaped_hover);
    return false;
  }

  int result_len = snprintf(
      NULL, 0, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
      escaped_hover);
  if (result_len < 0 || (size_t)result_len > LSP_HOVER_MAX_JSON_SIZE) {
    free(escaped_hover);
    return false;
  }

  char *result = malloc((size_t)result_len + 1);
  if (!result) {
    free(escaped_hover);
    return false;
  }

  int result_written =
      snprintf(result, (size_t)result_len + 1,
               "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
               escaped_hover);
  free(escaped_hover);

  if (result_written != result_len) {
    free(result);
    return false;
  }

  send_response(id, result);
  free(result);
  return true;
}

void handle_hover(const char *id, const char *body) {
  if (!g_doc || !g_doc->text) {
    send_response(id, "null");
    return;
  }

  char *line_str = json_get_nested_value(body, "params.position.line");
  char *character_str =
      json_get_nested_value(body, "params.position.character");

  if (!line_str || !character_str) {
    send_response(id, "null");
    free(line_str);
    free(character_str);
    return;
  }

  size_t line, character;
  if (!safe_strtoul(line_str, &line) || !safe_strtoul(character_str, &character)) {
    free(line_str);
    free(character_str);
    send_response(id, "null");
    return;
  }
  free(line_str);
  free(character_str);

  // Find word at position
  char *word = get_word_at_position(g_doc->text, line, character);
  if (!word) {
    send_response(id, "null");
    return;
  }

  // Handle module.function syntax
  char *dot = strchr(word, '.');
  if (dot) {
    // Extract module name and function name
    size_t module_len = (size_t)(dot - word);
    char *module_name = malloc(module_len + 1);
    if (module_name) {
      strncpy(module_name, word, module_len);
      module_name[module_len] = '\0';
      const char *func_name = dot + 1;

      // Check if it's a built-in module function
      if (strcmp(module_name, "math") == 0 ||
          strcmp(module_name, "regex") == 0) {
        // For built-in modules, show function info
        free(module_name);
        free(word);
        send_response(id, "null"); // Could enhance this later
        return;
      }

      const char *method_doc = method_description(func_name);
      if (method_doc) {
        char hover_text[512];
        snprintf(hover_text, sizeof(hover_text),
                 "**method** %s(...)\n\n%s\n\n"
                 "The expression before the dot is passed as the first argument.",
                 func_name, method_doc);
        bool sent = send_markdown_hover_response(id, hover_text);
        free(module_name);
        free(word);
        if (!sent) {
          send_response(id, "null");
        }
        return;
      }

      // Check if it's a file-based module
      ImportedModule *mod = g_doc ? g_doc->imported_modules : NULL;
      while (mod) {
        if (mod->name && strcmp(mod->name, module_name) == 0) {
          // Load exports if needed
          if (!mod->exports && mod->file_path) {
            mod->exports = load_module_exports(mod->file_path);
          }

          // Find the function in exports
          Symbol *func_sym = mod->exports;
          while (func_sym) {
            if (func_sym->type == SYMBOL_FUNCTION &&
                strcmp(func_sym->name, func_name) == 0) {
              // Build hover info for the function
              char *hover_text = NULL;
              size_t hover_len = 0;
              size_t hover_capacity = 0;
              bool hover_ok = hover_appendf(
                  &hover_text, &hover_len, &hover_capacity,
                  LSP_HOVER_MAX_MARKDOWN_SIZE,
                  "**function** `%s.%s`\n\n**Module:** `%s`\n\n", module_name,
                  func_name, module_name);

              // Show parameter information
              if (hover_ok && func_sym->param_count > 0 && func_sym->param_names) {
                hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                         LSP_HOVER_MAX_MARKDOWN_SIZE,
                                         "**Parameters:**\n");

                for (size_t i = 0; i < func_sym->param_count && hover_ok; i++) {
                  const char *param_name = func_sym->param_names[i]
                                               ? func_sym->param_names[i]
                                               : "?";
                  bool is_required = i < func_sym->required_param_count;
                  bool is_variadic = func_sym->has_variadic && i == func_sym->param_count - 1;

                  if (is_variadic) {
                    hover_ok = hover_appendf(
                        &hover_text, &hover_len, &hover_capacity,
                        LSP_HOVER_MAX_MARKDOWN_SIZE, "- `...%s` (variadic)\n",
                        param_name);
                  } else if (!is_required) {
                    hover_ok = hover_appendf(
                        &hover_text, &hover_len, &hover_capacity,
                        LSP_HOVER_MAX_MARKDOWN_SIZE, "- `%s` (optional)\n",
                        param_name);
                  } else {
                    hover_ok = hover_appendf(
                        &hover_text, &hover_len, &hover_capacity,
                        LSP_HOVER_MAX_MARKDOWN_SIZE, "- `%s` (required)\n",
                        param_name);
                  }
                }
              } else if (hover_ok) {
                hover_ok = hover_appendf(
                    &hover_text, &hover_len, &hover_capacity,
                    LSP_HOVER_MAX_MARKDOWN_SIZE, "**Parameters:** %zu\n",
                    func_sym->param_count);
              }

              bool sent = hover_ok &&
                          send_markdown_hover_response(id, hover_text ? hover_text : "");
              free(hover_text);
              free(module_name);
              free(word);
              if (!sent) {
                send_response(id, "null");
              }
              return;
            }
            func_sym = func_sym->next;
          }

          // Function not found in module
          free(module_name);
          free(word);
          send_response(id, "null");
          return;
        }
        mod = mod->next;
      }

      free(module_name);
    }
    free(word);
    send_response(id, "null");
    return;
  }

  // Check if it's a built-in module
  const char *module_desc = get_module_description(word);
  if (module_desc) {
    char *escaped_name = malloc(strlen(word) * 2 + 1);
    if (!escaped_name) {
      free(word);
      send_response(id, "null");
      return;
    }
    json_escape(word, escaped_name, strlen(word) * 2 + 1);

    char *hover_text = NULL;
    size_t hover_len = 0;
    size_t hover_capacity = 0;
    bool hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                  LSP_HOVER_MAX_MARKDOWN_SIZE,
                                  "**module** `%s`\n\n%s", escaped_name,
                                  module_desc);
    bool sent = hover_ok &&
                send_markdown_hover_response(id, hover_text ? hover_text : "");

    free(hover_text);
    free(escaped_name);
    free(word);
    if (!sent) {
      send_response(id, "null");
    }
    return;
  }

  // Check if it's a file-based module
  if (g_doc) {
    ImportedModule *mod = g_doc->imported_modules;
    while (mod) {
      if (mod->name && strcmp(mod->name, word) == 0) {
        // Get module hover info
        char *module_info = get_module_hover_info(mod);
        if (module_info) {
          bool sent = send_markdown_hover_response(id, module_info);
          free(module_info);
          free(word);
          if (!sent) {
            send_response(id, "null");
          }
          return;
        }
        break;
      }
      mod = mod->next;
    }
  }

  // Find symbol
  Symbol *sym = find_symbol_at_position(word, line, character);
  free(word);

  if (!sym) {
    send_response(id, "null");
    return;
  }

  // Build hover info
  char *hover_text = NULL;
  size_t hover_len = 0;
  size_t hover_capacity = 0;
  bool hover_ok = true;
  const char *type_str = "variable";
  if (sym->type == SYMBOL_FUNCTION)
    type_str = "function";
  else if (sym->type == SYMBOL_PARAMETER)
    type_str = "parameter";
  else if (sym->type == SYMBOL_TYPE_ALIAS)
    type_str = "type alias";

  char *escaped_name = malloc(strlen(sym->name) * 2 + 1);
  if (!escaped_name) {
    send_response(id, "null");
    return;
  }
  json_escape(sym->name, escaped_name, strlen(sym->name) * 2 + 1);

  if (sym->type == SYMBOL_FUNCTION) {
    // Build function signature with parameter info
    hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                             LSP_HOVER_MAX_MARKDOWN_SIZE,
                             "**function** `%s`\n\n", escaped_name);

    // Show parameter information
    if (hover_ok && sym->param_count > 0) {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "**Parameters:**\n");

      for (size_t i = 0; i < sym->param_count && hover_ok; i++) {
        const char *param_name = (sym->param_names && sym->param_names[i])
                                     ? sym->param_names[i]
                                     : "?";
        bool is_required = i < sym->required_param_count;
        bool is_variadic = sym->has_variadic && i == sym->param_count - 1;

        if (is_variadic) {
          hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                   LSP_HOVER_MAX_MARKDOWN_SIZE,
                                   "- `...%s` (variadic)\n", param_name);
        } else if (!is_required) {
          hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                   LSP_HOVER_MAX_MARKDOWN_SIZE,
                                   "- `%s` (optional, has default)\n",
                                   param_name);
        } else {
          hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                   LSP_HOVER_MAX_MARKDOWN_SIZE,
                                   "- `%s` (required)\n", param_name);
        }
      }
    } else if (hover_ok) {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "No parameters\n");
    }

    // Show summary
    if (hover_ok && sym->has_variadic) {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "\n*Accepts %zu or more argument%s*",
                               sym->required_param_count,
                               sym->required_param_count == 1 ? "" : "s");
    } else if (hover_ok && sym->required_param_count < sym->param_count) {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "\n*Accepts %zu to %zu argument%s*",
                               sym->required_param_count, sym->param_count,
                               sym->param_count == 1 ? "" : "s");
    }
  } else if (sym->type == SYMBOL_TYPE_ALIAS) {
    if (sym->type_name) {
      char *escaped_type = malloc(strlen(sym->type_name) * 2 + 1);
      if (escaped_type) {
        json_escape(sym->type_name, escaped_type,
                    strlen(sym->type_name) * 2 + 1);
        hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                 LSP_HOVER_MAX_MARKDOWN_SIZE,
                                 "**type alias** `%s`\n\nResolves to: `%s`",
                                 escaped_name, escaped_type);
        free(escaped_type);
      } else {
        hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                                 LSP_HOVER_MAX_MARKDOWN_SIZE,
                                 "**type alias** `%s`", escaped_name);
      }
    } else {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "**type alias** `%s`", escaped_name);
    }
  } else if (sym->type_name) {
    char *escaped_type = malloc(strlen(sym->type_name) * 2 + 1);
    if (escaped_type) {
      json_escape(sym->type_name, escaped_type, strlen(sym->type_name) * 2 + 1);
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE,
                               "**%s** `%s`\n\nType: `%s`\n%s", type_str,
                               escaped_name, escaped_type,
                               sym->is_mutable ? "Mutable" : "Immutable");
      free(escaped_type);
    } else {
      hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                               LSP_HOVER_MAX_MARKDOWN_SIZE, "**%s** `%s`",
                               type_str, escaped_name);
    }
  } else {
    hover_ok = hover_appendf(&hover_text, &hover_len, &hover_capacity,
                             LSP_HOVER_MAX_MARKDOWN_SIZE, "**%s** `%s`\n%s",
                             type_str, escaped_name,
                             sym->is_mutable ? "Mutable" : "Immutable");
  }
  free(escaped_name);
  if (hover_ok && send_markdown_hover_response(id, hover_text ? hover_text : "")) {
    free(hover_text);
    return;
  }

  free(hover_text);
  send_response(id, "null");
}
