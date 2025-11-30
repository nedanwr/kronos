/**
 * @file tokenizer.c
 * @brief Lexical analyzer for Kronos source code
 *
 * Converts Kronos source code into a stream of tokens. Handles:
 * - Keywords (set, let, if, for, while, etc.)
 * - Literals (numbers, strings, f-strings, booleans, null)
 * - Operators (plus, minus, times, divided by, is equal, etc.)
 * - Identifiers and variable names
 * - Indentation tracking (spaces and tabs)
 * - Newlines and special characters
 */

#define _POSIX_C_SOURCE 200809L
#include "tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Tab width in spaces for indentation calculation */
#define TOKENIZER_TAB_WIDTH 8

/**
 * Token type names for debugging output
 * Must match TokenType enum order exactly
 */
static const char *token_type_names[] = {
    "NUMBER",    "STRING", "FSTRING", "SET",    "LET",     "TO",     "AS",
    "IF",        "ELSE",   "ELSE_IF", "FOR",    "WHILE",   "BREAK",  "CONTINUE",
    "IN",        "RANGE",  "LIST",    "AT",     "FROM",    "END",    "FUNCTION",
    "WITH",      "CALL",   "RETURN",  "IMPORT", "TRUE",    "FALSE",  "NULL",
    "UNDEFINED", "IS",     "EQUAL",   "NOT",    "GREATER", "LESS",   "THAN",
    "AND",       "OR",     "PRINT",   "PLUS",   "MINUS",   "TIMES",  "DIVIDED",
    "BY",        "NAME",   "COLON",   "COMMA",  "NEWLINE", "INDENT", "EOF"};

/**
 * @brief Allocate and initialize a new token array
 *
 * Creates a dynamically-growing array to hold tokens during tokenization.
 * Starts with capacity 32 and grows as needed.
 *
 * @return New token array, or NULL on allocation failure
 */
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

/**
 * @brief Add a token to the array
 *
 * Automatically grows the array if needed. On failure, frees the token's
 * text if it was allocated.
 *
 * @param arr Token array to add to
 * @param token Token to add (text will be owned by array)
 * @return true on success, false on allocation failure
 */
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

/**
 * @brief Determine if a string matches a Kronos keyword
 *
 * Checks the string against all known keywords. If no match is found,
 * returns TOK_NAME indicating it's an identifier.
 *
 * @param text String to check (not null-terminated)
 * @param len Length of the string
 * @return Token type (keyword type or TOK_NAME if not a keyword)
 */
static TokenType match_keyword(const char *text, size_t len) {
  struct {
    const char *keyword;
    TokenType type;
  } keywords[] = {
      {"set", TOK_SET},
      {"let", TOK_LET},
      {"to", TOK_TO},
      {"as", TOK_AS},
      {"if", TOK_IF},
      {"else", TOK_ELSE},
      {"for", TOK_FOR},
      {"while", TOK_WHILE},
      {"break", TOK_BREAK},
      {"continue", TOK_CONTINUE},
      {"in", TOK_IN},
      {"range", TOK_RANGE},
      {"list", TOK_LIST},
      {"map", TOK_MAP},
      {"at", TOK_AT},
      {"from", TOK_FROM},
      {"end", TOK_END},
      {"function", TOK_FUNCTION},
      {"with", TOK_WITH},
      {"call", TOK_CALL},
      {"return", TOK_RETURN},
      {"import", TOK_IMPORT},
      {"true", TOK_TRUE},
      {"false", TOK_FALSE},
      {"null", TOK_NULL},
      {"undefined", TOK_UNDEFINED},
      {"is", TOK_IS},
      {"equal", TOK_EQUAL},
      {"not", TOK_NOT},
      {"greater", TOK_GREATER},
      {"less", TOK_LESS},
      {"than", TOK_THAN},
      {"and", TOK_AND},
      {"or", TOK_OR},
      {"print", TOK_PRINT},
      {"plus", TOK_PLUS},
      {"minus", TOK_MINUS},
      {"times", TOK_TIMES},
      {"divided", TOK_DIVIDED},
      {"by", TOK_BY},
  };

  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if (strlen(keywords[i].keyword) == len &&
        strncmp(text, keywords[i].keyword, len) == 0) {
      return keywords[i].type;
    }
  }

  return TOK_NAME;
}

/**
 * @brief Tokenize a single line of source code
 *
 * Processes one line, extracting all tokens. Handles:
 * - Numbers (integers and floats, with optional leading + or -)
 * - Strings and f-strings (with escape sequences)
 * - Keywords and identifiers
 * - Operators and punctuation
 * - Indentation tokens
 *
 * @param arr Token array to append tokens to
 * @param line The line to tokenize (should not include leading whitespace)
 * @param indent Indentation level in spaces (already calculated)
 * @return true on success, false on error (e.g., unterminated string)
 */
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

    // Tokenize numbers (integers and floating-point)
    // Supports: 42, 3.14, 0.5, -42, +42, -3.14, etc.
    // Note: The negative sign (-) is part of the number literal (denotes
    // negative value), not a subtraction operator. Subtraction is handled by
    // the 'minus' keyword. Check for optional leading '+' or '-' when followed
    // by a digit
    bool has_sign = false;
    if ((line[col] == '+' || line[col] == '-') && (col + 1 < len) &&
        isdigit(line[col + 1])) {
      has_sign = true;
    }
    if (has_sign || isdigit(line[col])) {
      size_t start = col;
      if (has_sign) {
        col++; // consume sign
      }
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
    // F-strings allow embedded expressions like f"Hello {name}"
    bool is_fstring = false;
    if (col + 1 < len && line[col] == 'f' &&
        (line[col + 1] == '"' || line[col + 1] == '\'')) {
      is_fstring = true;
      col++; // Skip 'f'
    }

    // Tokenize string literals (handles escape sequences)
    // Supports both single and double quotes
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

    // Tokenize identifiers and keywords
    // Identifiers can contain letters, digits, underscores, and dots
    // (dots are allowed for module.function syntax like math.sqrt)
    if (isalpha(line[col]) || line[col] == '_') {
      size_t start = col;
      while (col < len &&
             (isalnum(line[col]) || line[col] == '_' || line[col] == '.')) {
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

    // Tokenize single-character punctuation
    // Colon is used for if/for/while statement headers
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

    // Unknown character - skip it (allows graceful degradation)
    // In a strict mode, this could be an error
    col++;
  }

  // Add newline token to mark end of line (if line had content)
  // Empty lines don't get newline tokens to avoid clutter
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

/**
 * @brief Free a tokenization error structure
 *
 * Releases memory allocated for an error message and the error structure
 * itself.
 *
 * @param err Error structure to free (safe to pass NULL)
 */
void tokenize_error_free(TokenizeError *err) {
  if (!err)
    return;
  free(err->message);
  free(err);
}

/**
 * @brief Report a tokenization error
 *
 * Creates and stores an error structure with message and position information.
 * Only sets the error if out_err is provided and no error is already set.
 *
 * @param out_err Pointer to error output (can be NULL to ignore)
 * @param message Error message (will be copied)
 * @param line Line number where error occurred (1-indexed)
 * @param column Column number where error occurred (1-indexed)
 */
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

/**
 * @brief Tokenize Kronos source code
 *
 * Main entry point for lexical analysis. Splits source into lines, calculates
 * indentation, and tokenizes each line. Handles mixed indentation errors
 * (spaces and tabs in the same block).
 *
 * @param source Complete source code to tokenize (must not be NULL)
 * @param out_err Optional pointer to receive error information
 * @return Token array on success, NULL on error
 */
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

  // Process source line by line
  const char *line_start = source;
  const char *line_end;
  size_t line_number = 1;

  while (*line_start) {
    // Find the end of the current line (newline or end of string)
    line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }

    // Calculate line length
    size_t line_len = line_end - line_start;

    // Calculate indentation level
    // Tabs are treated as TOKENIZER_TAB_WIDTH spaces
    // Mixed spaces and tabs in the same block is an error
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
        break; // End of leading whitespace
      }

      // Error: mixing spaces and tabs in indentation
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

    // Extract line content (after leading whitespace)
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

  // Add end-of-file token to mark completion
  Token eof = {TOK_EOF, NULL, 0, 0};
  if (!token_array_add(arr, eof)) {
    tokenizer_report_error(
        out_err, "Failed to append EOF token (out of memory)", line_number, 1);
    token_array_free(arr);
    return NULL;
  }

  return arr;
}

/**
 * @brief Free resources owned by a single token
 *
 * Releases the token's text string if it was allocated.
 *
 * @param token Token to free (safe to pass NULL)
 */
void token_free(Token *token) {
  if (!token)
    return;
  free((char *)token->text);
  token->text = NULL;
}

/**
 * @brief Free a token array and all its tokens
 *
 * Releases all token text strings and the array structure itself.
 *
 * @param array Token array to free (safe to pass NULL)
 */
void token_array_free(TokenArray *array) {
  if (!array)
    return;

  for (size_t i = 0; i < array->count; i++) {
    free((char *)array->tokens[i].text);
  }
  free(array->tokens);
  free(array);
}

/**
 * @brief Print a token for debugging
 *
 * Outputs the token type and value in a human-readable format.
 * Useful for debugging tokenization issues.
 *
 * @param token Token to print (safe to pass NULL)
 */
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
