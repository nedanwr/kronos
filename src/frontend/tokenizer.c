#define _POSIX_C_SOURCE 200809L
#include "tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Tabs count as this many spaces when computing indentation
#define TOKENIZER_TAB_WIDTH 8

// Token type to string (for debugging)
// Must match TokenType enum order exactly
static const char *token_type_names[] = {
    "NUMBER",  "STRING", "FSTRING",  "SET",   "LET",   "TO",      "AS",
    "IF",      "FOR",    "WHILE",    "IN",    "RANGE", "LIST",    "AT",
    "FROM",    "END",    "FUNCTION", "WITH",  "CALL",  "RETURN",  "TRUE",
    "FALSE",   "NULL",   "IS",       "EQUAL", "NOT",   "GREATER", "LESS",
    "THAN",    "AND",    "OR",       "PRINT", "PLUS",  "MINUS",   "TIMES",
    "DIVIDED", "BY",     "NAME",     "COLON", "COMMA", "NEWLINE", "INDENT",
    "EOF"};

// Helper to create token array
static TokenArray *token_array_new(void) {
  TokenArray *arr = malloc(sizeof(TokenArray));
  if (!arr)
    return NULL;

  arr->capacity = 32;
  arr->count = 0;
  arr->tokens = malloc(sizeof(Token) * arr->capacity);
  if (!arr->tokens) {
    free(arr);
    return NULL;
  }

  return arr;
}

// Helper to add token
static bool token_array_add(TokenArray *arr, Token token) {
  if (!arr)
    return false;

  if (arr->count >= arr->capacity) {
    size_t new_capacity = arr->capacity ? arr->capacity * 2 : 1;
    Token *new_tokens = realloc(arr->tokens, sizeof(Token) * new_capacity);
    if (!new_tokens) {
      fprintf(stderr, "Fatal: tokenizer failed to grow token array\n");
      if (token.text)
        free((void *)token.text);
      return false;
    }
    arr->tokens = new_tokens;
    arr->capacity = new_capacity;
  }
  arr->tokens[arr->count++] = token;
  return true;
}

// Check if string matches keyword
static TokenType match_keyword(const char *text, size_t len) {
  struct {
    const char *keyword;
    TokenType type;
  } keywords[] = {
      {"set", TOK_SET},         {"let", TOK_LET},
      {"to", TOK_TO},           {"as", TOK_AS},
      {"if", TOK_IF},           {"for", TOK_FOR},
      {"while", TOK_WHILE},     {"in", TOK_IN},
      {"range", TOK_RANGE},     {"list", TOK_LIST},
      {"at", TOK_AT},           {"from", TOK_FROM},
      {"end", TOK_END},         {"function", TOK_FUNCTION},
      {"with", TOK_WITH},       {"call", TOK_CALL},
      {"return", TOK_RETURN},   {"import", TOK_IMPORT},
      {"true", TOK_TRUE},
      {"false", TOK_FALSE},     {"null", TOK_NULL},
      {"is", TOK_IS},           {"equal", TOK_EQUAL},
      {"not", TOK_NOT},         {"greater", TOK_GREATER},
      {"less", TOK_LESS},       {"than", TOK_THAN},
      {"and", TOK_AND},         {"or", TOK_OR},
      {"print", TOK_PRINT},     {"plus", TOK_PLUS},
      {"minus", TOK_MINUS},     {"times", TOK_TIMES},
      {"divided", TOK_DIVIDED}, {"by", TOK_BY},
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if (strlen(keywords[i].keyword) == len &&
        strncmp(text, keywords[i].keyword, len) == 0) {
      return keywords[i].type;
    }
  }

  return TOK_NAME;
}

// Tokenize a single line
static bool tokenize_line(TokenArray *arr, const char *line, int indent) {
  size_t len = strlen(line);
  size_t col = 0;

  // Add indent token if line is not empty
  if (len > 0) {
    Token tok = {TOK_INDENT, NULL, 0, indent};
    if (!token_array_add(arr, tok))
      return false;
  }

  while (col < len) {
    // Skip whitespace
    while (col < len && (line[col] == ' ' || line[col] == '\t')) {
      col++;
    }

    if (col >= len)
      break;

    // Numbers
    if (isdigit(line[col])) {
      size_t start = col;
      while (col < len && isdigit(line[col])) {
        col++;
      }
      if (col < len && line[col] == '.' && (col + 1 < len) &&
          isdigit(line[col + 1])) {
        col++; // consume '.'
        while (col < len && isdigit(line[col])) {
          col++;
        }
      }
      Token tok = {TOK_NUMBER, NULL, col - start, 0};
      char *text_buf = malloc(tok.length + 1);
      if (!text_buf) {
        fprintf(stderr, "Failed to allocate memory for number literal\n");
        return false;
      }
      memcpy(text_buf, line + start, tok.length);
      text_buf[tok.length] = '\0';
      tok.text = text_buf;
      if (!token_array_add(arr, tok)) {
        free(text_buf);
        return false;
      }
      continue;
    }

    // Check for f-string prefix (f"..." or f'...')
    bool is_fstring = false;
    if (col + 1 < len && line[col] == 'f' &&
        (line[col + 1] == '"' || line[col + 1] == '\'')) {
      is_fstring = true;
      col++; // Skip 'f'
    }

    // Strings with escape handling
    if (line[col] == '"' || line[col] == '\'') {
      char quote_char = line[col];
      size_t content_start = col + 1;
      size_t cursor = content_start;
      bool closed = false;
      while (cursor < len) {
        if (line[cursor] == '\\') {
          cursor++;
          if (cursor < len) {
            cursor++;
          } else {
            break;
          }
        } else if (line[cursor] == quote_char) {
          closed = true;
          break;
        } else {
          cursor++;
        }
      }

      if (!closed) {
        fprintf(stderr, "Unterminated string literal\n");
        return false;
      }

      size_t content_len = cursor - content_start;
      Token tok = {is_fstring ? TOK_FSTRING : TOK_STRING, NULL, content_len, 0};
      char *text_buf = malloc(content_len + 1);
      if (!text_buf) {
        fprintf(stderr, "Failed to allocate memory for string literal\n");
        return false;
      }
      memcpy(text_buf, line + content_start, content_len);
      text_buf[content_len] = '\0';
      tok.text = text_buf;
      if (!token_array_add(arr, tok)) {
        free(text_buf);
        return false;
      }
      col = cursor + 1; // Skip closing quote
      continue;
    }

    // Names and keywords (allow dots for module.function syntax)
    if (isalpha(line[col]) || line[col] == '_') {
      size_t start = col;
      while (col < len && (isalnum(line[col]) || line[col] == '_' || line[col] == '.')) {
        col++;
      }
      size_t word_len = col - start;
      TokenType type = match_keyword(line + start, word_len);
      Token tok = {type, NULL, word_len, 0};
      char *text_buf = malloc(tok.length + 1);
      if (!text_buf) {
        fprintf(stderr, "Failed to allocate memory for identifier token\n");
        return false;
      }
      strncpy(text_buf, line + start, tok.length);
      text_buf[tok.length] = '\0';
      tok.text = text_buf;
      if (!token_array_add(arr, tok)) {
        free(text_buf);
        return false;
      }
      continue;
    }

    // Single character tokens
    if (line[col] == ':') {
      Token tok = {TOK_COLON, NULL, 1, 0};
      tok.text = strdup(":");
      if (!tok.text) {
        fprintf(stderr, "Failed to allocate colon token\n");
        return false;
      }
      if (!token_array_add(arr, tok)) {
        free((void *)tok.text);
        return false;
      }
      col++;
      continue;
    }

    if (line[col] == ',') {
      Token tok = {TOK_COMMA, NULL, 1, 0};
      tok.text = strdup(",");
      if (!tok.text) {
        fprintf(stderr, "Failed to allocate comma token\n");
        return false;
      }
      if (!token_array_add(arr, tok)) {
        free((void *)tok.text);
        return false;
      }
      col++;
      continue;
    }

    // Unknown character, skip it
    col++;
  }

  // Add newline token if line had content
  if (len > 0) {
    char *newline_text = strdup("\n");
    if (!newline_text) {
      fprintf(stderr, "Failed to allocate newline token text\n");
      return false;
    }
    Token tok = {TOK_NEWLINE, newline_text, 1, 0};
    if (!token_array_add(arr, tok)) {
      free(newline_text);
      return false;
    }
  }
  return true;
}

// Free a TokenizeError structure
void tokenize_error_free(TokenizeError *err) {
  if (!err)
    return;
  free(err->message);
  free(err);
}

// Helper to set tokenize error info
static void tokenizer_report_error(TokenizeError **out_err, const char *message,
                                   size_t line, size_t column) {
  if (!out_err || *out_err)
    return;

  TokenizeError *err = malloc(sizeof(TokenizeError));
  if (!err)
    return;

  err->message = strdup(message ? message : "Tokenizer error");
  if (!err->message) {
    free(err);
    return;
  }
  err->line = line;
  err->column = column;
  *out_err = err;
}

// Tokenize source code
TokenArray *tokenize(const char *source, TokenizeError **out_err) {
  // Initialize error output
  if (out_err)
    *out_err = NULL;

  // Validate input
  if (!source) {
    if (out_err) {
      TokenizeError *err = malloc(sizeof(TokenizeError));
      if (err) {
        err->message = strdup("Source code must not be NULL");
        err->line = 0;
        err->column = 0;
        *out_err = err;
      }
    }
    return NULL;
  }

  TokenArray *arr = token_array_new();
  if (!arr) {
    if (out_err) {
      TokenizeError *err = malloc(sizeof(TokenizeError));
      if (err) {
        err->message = strdup("Failed to allocate TokenArray");
        err->line = 0;
        err->column = 0;
        *out_err = err;
      }
    }
    return NULL;
  }

  // Split source into lines
  const char *line_start = source;
  const char *line_end;
  size_t line_number = 1;

  while (*line_start) {
    // Find end of line
    line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }

    // Calculate line length
    size_t line_len = line_end - line_start;

    // Calculate indent treating tabs as TOKENIZER_TAB_WIDTH spaces
    int indent = 0;
    bool saw_space = false;
    bool saw_tab = false;
    size_t i = 0;
    for (; i < line_len; i++) {
      char c = line_start[i];
      if (c == ' ') {
        saw_space = true;
        indent++;
      } else if (c == '\t') {
        saw_tab = true;
        indent += TOKENIZER_TAB_WIDTH;
      } else {
        break;
      }

      if (saw_space && saw_tab) {
        fprintf(stderr, "Mixed spaces and tabs in indentation on line %zu\n",
                line_number);
        tokenizer_report_error(out_err,
                               "Mixed indentation (spaces and tabs are not "
                               "allowed in the same block)",
                               line_number, 1);
        token_array_free(arr);
        return NULL;
      }
    }

    // Create a copy of the line (stripped)
    const char *content_start = line_start + i;
    size_t content_len = line_len - i;

    if (content_len > 0) {
      char *line = malloc(content_len + 1);
      if (!line) {
        fprintf(stderr, "Failed to allocate memory for line copy on line %zu\n",
                line_number);
        tokenizer_report_error(out_err,
                               "Failed to allocate memory while tokenizing",
                               line_number, 1);
        token_array_free(arr);
        return NULL;
      }
      strncpy(line, content_start, content_len);
      line[content_len] = '\0';

      if (!tokenize_line(arr, line, indent)) {
        tokenizer_report_error(
            out_err, "Failed to tokenize line (out of memory)", line_number, 1);
        free(line);
        token_array_free(arr);
        return NULL;
      }
      free(line);
    }

    // Move to next line
    line_start = line_end;
    if (*line_start == '\n')
      line_start++;
    line_number++;
  }

  // Add EOF token
  Token eof = {TOK_EOF, NULL, 0, 0};
  if (!token_array_add(arr, eof)) {
    tokenizer_report_error(
        out_err, "Failed to append EOF token (out of memory)", line_number, 1);
    token_array_free(arr);
    return NULL;
  }

  return arr;
}

// Free a single Token's resources
void token_free(Token *token) {
  if (!token)
    return;
  free((char *)token->text);
  token->text = NULL;
}

// Free token array
void token_array_free(TokenArray *array) {
  if (!array)
    return;

  for (size_t i = 0; i < array->count; i++) {
    free((char *)array->tokens[i].text);
  }
  free(array->tokens);
  free(array);
}

// Print token (debug)
void token_print(Token *token) {
  if (!token)
    return;

  // Bounds check before indexing token_type_names
  const char *type_name;
  size_t type_names_count =
      sizeof(token_type_names) / sizeof(token_type_names[0]);
  if (token->type < type_names_count) {
    type_name = token_type_names[token->type];
  } else {
    type_name = "UNKNOWN";
  }

  printf("%-12s", type_name);
  if (token->type == TOK_INDENT) {
    printf(" (indent=%d)", token->indent_level);
  } else if (token->text) {
    printf(" '%s'", token->text);
  }
  printf("\n");
}
