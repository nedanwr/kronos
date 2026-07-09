/**
 * @file lsp_hover.c
 * @brief Hover information for LSP server
 */

#include "lsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern DocumentState *g_doc;

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
              char hover_text[1024];
              size_t pos = 0;
              int ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                       "**function** `%s.%s`\n\n**Module:** `%s`\n\n",
                       module_name, func_name, module_name);
              if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
                pos += (size_t)ret;
              }

              // Show parameter information
              if (func_sym->param_count > 0 && func_sym->param_names) {
                ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                               "**Parameters:**\n");
                if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
                  pos += (size_t)ret;
                }

                for (size_t i = 0; i < func_sym->param_count && pos < sizeof(hover_text) - 1; i++) {
                  const char *param_name = func_sym->param_names[i]
                                               ? func_sym->param_names[i]
                                               : "?";
                  bool is_required = i < func_sym->required_param_count;
                  bool is_variadic = func_sym->has_variadic && i == func_sym->param_count - 1;

                  if (is_variadic) {
                    ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                                   "- `...%s` (variadic)\n", param_name);
                  } else if (!is_required) {
                    ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                                   "- `%s` (optional)\n", param_name);
                  } else {
                    ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                                   "- `%s` (required)\n", param_name);
                  }
                  if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
                    pos += (size_t)ret;
                  }
                }
              } else {
                ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                               "**Parameters:** %zu\n", func_sym->param_count);
                if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
                  pos += (size_t)ret;
                }
              }

              char escaped_hover[2048];
              json_escape_markdown(hover_text, escaped_hover, sizeof(escaped_hover));

              char result[2048];
              snprintf(
                  result, sizeof(result),
                  "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                  escaped_hover);
              free(module_name);
              free(word);
              send_response(id, result);
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

    // For markdown, we need to escape special characters but preserve newlines
    // Build the hover text with proper escaping
    char hover_text[2048];
    snprintf(hover_text, sizeof(hover_text), "**module** `%s`\n\n%s",
             escaped_name, module_desc);

    // Escape for JSON but preserve newlines as \n (not \\n)
    char escaped_hover[4096];
    json_escape_markdown(hover_text, escaped_hover, sizeof(escaped_hover));

    char result[4096];
    snprintf(result, sizeof(result),
             "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
             escaped_hover);
    free(escaped_name);
    free(word);
    send_response(id, result);
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
          char escaped_hover[4096];
          json_escape_markdown(module_info, escaped_hover, sizeof(escaped_hover));

          char result[4096];
          snprintf(result, sizeof(result),
                   "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
                   escaped_hover);
          free(module_info);
          free(word);
          send_response(id, result);
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
  char hover_text[1024];
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
    size_t pos = 0;
    int ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                       "**function** `%s`\n\n", escaped_name);
    if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
      pos += (size_t)ret;
    }

    // Show parameter information
    if (sym->param_count > 0) {
      ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                     "**Parameters:**\n");
      if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
        pos += (size_t)ret;
      }

      for (size_t i = 0; i < sym->param_count && pos < sizeof(hover_text) - 1; i++) {
        const char *param_name = (sym->param_names && sym->param_names[i])
                                     ? sym->param_names[i]
                                     : "?";
        bool is_required = i < sym->required_param_count;
        bool is_variadic = sym->has_variadic && i == sym->param_count - 1;

        if (is_variadic) {
          ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                         "- `...%s` (variadic)\n", param_name);
        } else if (!is_required) {
          ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                         "- `%s` (optional, has default)\n", param_name);
        } else {
          ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                         "- `%s` (required)\n", param_name);
        }
        if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
          pos += (size_t)ret;
        }
      }
    } else {
      ret = snprintf(hover_text + pos, sizeof(hover_text) - pos,
                     "No parameters\n");
      if (ret > 0 && (size_t)ret < sizeof(hover_text) - pos) {
        pos += (size_t)ret;
      }
    }

    // Show summary
    if (sym->has_variadic) {
      snprintf(hover_text + pos, sizeof(hover_text) - pos,
               "\n*Accepts %zu or more argument%s*",
               sym->required_param_count,
               sym->required_param_count == 1 ? "" : "s");
    } else if (sym->required_param_count < sym->param_count) {
      snprintf(hover_text + pos, sizeof(hover_text) - pos,
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
        snprintf(hover_text, sizeof(hover_text),
                 "**type alias** `%s`\n\nResolves to: `%s`", escaped_name,
                 escaped_type);
        free(escaped_type);
      } else {
        snprintf(hover_text, sizeof(hover_text), "**type alias** `%s`",
                 escaped_name);
      }
    } else {
      snprintf(hover_text, sizeof(hover_text), "**type alias** `%s`",
               escaped_name);
    }
  } else if (sym->type_name) {
    char *escaped_type = malloc(strlen(sym->type_name) * 2 + 1);
    if (escaped_type) {
      json_escape(sym->type_name, escaped_type, strlen(sym->type_name) * 2 + 1);
      snprintf(hover_text, sizeof(hover_text), "**%s** `%s`\n\nType: `%s`\n%s",
               type_str, escaped_name, escaped_type,
               sym->is_mutable ? "Mutable" : "Immutable");
      free(escaped_type);
    } else {
      snprintf(hover_text, sizeof(hover_text), "**%s** `%s`", type_str,
               escaped_name);
    }
  } else {
    snprintf(hover_text, sizeof(hover_text), "**%s** `%s`\n%s", type_str,
             escaped_name, sym->is_mutable ? "Mutable" : "Immutable");
  }
  free(escaped_name);

  char escaped_hover[2048];
  json_escape_markdown(hover_text, escaped_hover, sizeof(escaped_hover));

  char result[2048];
  snprintf(result, sizeof(result),
           "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}",
           escaped_hover);
  send_response(id, result);
}
