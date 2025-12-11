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
 * @brief Check if a byte is a valid UTF-8 continuation byte
 *
 * UTF-8 continuation bytes have pattern 10xxxxxx (0x80-0xBF)
 *
 * @param byte The byte to check
 * @return true if this is a valid UTF-8 continuation byte
 */
static bool is_utf8_continuation_byte(unsigned char byte) {
  return (byte >= 0x80 && byte < 0xC0);
}

/**
 * @brief Get the length of a UTF-8 sequence starting at a given byte
 *
 * @param start_byte The first byte of the UTF-8 sequence
 * @return Number of bytes in the sequence (1-4), or 0 if invalid
 */
static size_t utf8_sequence_length(unsigned char start_byte) {
  if (start_byte < 0x80) {
    return 1; // ASCII
  } else if ((start_byte & 0xE0) == 0xC0) {
    return 2; // 110xxxxx
  } else if ((start_byte & 0xF0) == 0xE0) {
    return 3; // 1110xxxx
  } else if ((start_byte & 0xF8) == 0xF0) {
    return 4; // 11110xxx
  }
  return 0; // Invalid
}

/**
 * @brief Check if a character can start an identifier
 *
 * Identifiers can start with:
 * - ASCII letters (a-z, A-Z)
 * - Underscore (_)
 * - Valid UTF-8 sequences (Unicode letters)
 *
 * @param line The line buffer
 * @param col Current column position
 * @param len Length of the line
 * @return true if this position can start an identifier
 */
static bool can_start_identifier(const char *line, size_t col, size_t len) {
  if (col >= len)
    return false;

  unsigned char byte = (unsigned char)line[col];

  // ASCII letters and underscore
  if (isalpha(byte) || byte == '_')
    return true;

  // Check for valid UTF-8 multi-byte sequence
  if (byte >= 0x80) {
    size_t seq_len = utf8_sequence_length(byte);
    if (seq_len > 0 && col + seq_len <= len) {
      // Validate continuation bytes
      for (size_t i = 1; i < seq_len; i++) {
        if (!is_utf8_continuation_byte((unsigned char)line[col + i])) {
          return false;
        }
      }
      // Accept any valid UTF-8 sequence as identifier start
      // (Unicode letters, symbols, etc.)
      return true;
    }
  }

  return false;
}

/**
 * @brief Check if a character can continue an identifier
 *
 * Identifiers can continue with:
 * - ASCII letters and digits (a-z, A-Z, 0-9)
 * - Underscore (_)
 * - Dot (.) for module.function syntax
 * - Valid UTF-8 sequences (Unicode letters, digits, etc.)
 *
 * @param line The line buffer
 * @param col Current column position
 * @param len Length of the line
 * @return true if this position can continue an identifier
 */
static bool can_continue_identifier(const char *line, size_t col, size_t len) {
  if (col >= len)
    return false;

  unsigned char byte = (unsigned char)line[col];

  // ASCII alphanumeric, underscore, and dot
  if (isalnum(byte) || byte == '_' || byte == '.')
    return true;

  // Check for valid UTF-8 multi-byte sequence
  if (byte >= 0x80) {
    size_t seq_len = utf8_sequence_length(byte);
    if (seq_len > 0 && col + seq_len <= len) {
      // Validate continuation bytes
      for (size_t i = 1; i < seq_len; i++) {
        if (!is_utf8_continuation_byte((unsigned char)line[col + i])) {
          return false;
        }
      }
      // Accept any valid UTF-8 sequence as identifier continuation
      return true;
    }
  }

  return false;
}

/**
 * Token type names for debugging output
 * Must match TokenType enum order exactly
 */
static const char *token_type_names[] = {
    "NUMBER", "STRING",   "FSTRING",  "SET",     "LET",       "TO",
    "AS",     "IF",       "ELSE",     "ELSE_IF", "FOR",       "WHILE",
    "BREAK",  "CONTINUE", "IN",       "RANGE",   "LIST",      "AT",
    "FROM",   "END",      "FUNCTION", "WITH",    "CALL",      "RETURN",
    "IMPORT", "TRUE",     "FALSE",    "NULL",    "UNDEFINED", "IS",
    "EQUAL",  "NOT",      "GREATER",  "LESS",    "THAN",      "AND",
    "OR",     "PRINT",    "PLUS",     "MINUS",   "TIMES",     "DIVIDED",
    "BY",     "MOD",      "DELETE",   "TRY",     "CATCH",     "FINALLY",
    "RAISE",  "NAME",     "COLON",    "COMMA",   "NEWLINE",   "INDENT",
    "EOF"};

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

// Forward declarations
static bool process_escape_sequence(char escaped_char, char *out_char);
static void tokenizer_report_error(TokenizeError **out_err, const char *message,
                                   size_t line, size_t column);

/**
 * @brief Add a token to the array
 *
 * Automatically grows the array if needed. On failure, frees the token's
 * text if it was allocated.
 *
 * @param arr Token array to add to
 * @param token Token to add (text will be owned by array)
 * @param out_err Optional pointer to receive error information
 * @param line_number Line number for error reporting (1-based)
 * @param column Column number for error reporting (1-based)
 * @return true on success, false on allocation failure
 */
static bool token_array_add(TokenArray *arr, Token token,
                            TokenizeError **out_err, size_t line_number,
                            size_t column) {
  if (!arr)
    return false;

  if (arr->count >= arr->capacity) {
    size_t new_capacity = arr->capacity ? arr->capacity * 2 : 1;
    Token *new_tokens = realloc(arr->tokens, sizeof(Token) * new_capacity);
    if (!new_tokens) {
      tokenizer_report_error(out_err,
                             "Failed to grow token array (out of memory)",
                             line_number, column);
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
      {"set", TOK_SET},         {"let", TOK_LET},
      {"to", TOK_TO},           {"as", TOK_AS},
      {"if", TOK_IF},           {"else", TOK_ELSE},
      {"for", TOK_FOR},         {"while", TOK_WHILE},
      {"break", TOK_BREAK},     {"continue", TOK_CONTINUE},
      {"in", TOK_IN},           {"range", TOK_RANGE},
      {"list", TOK_LIST},       {"map", TOK_MAP},
      {"at", TOK_AT},           {"from", TOK_FROM},
      {"end", TOK_END},         {"function", TOK_FUNCTION},
      {"with", TOK_WITH},       {"call", TOK_CALL},
      {"return", TOK_RETURN},   {"import", TOK_IMPORT},
      {"true", TOK_TRUE},       {"false", TOK_FALSE},
      {"null", TOK_NULL},       {"undefined", TOK_UNDEFINED},
      {"is", TOK_IS},           {"equal", TOK_EQUAL},
      {"not", TOK_NOT},         {"greater", TOK_GREATER},
      {"less", TOK_LESS},       {"than", TOK_THAN},
      {"and", TOK_AND},         {"or", TOK_OR},
      {"print", TOK_PRINT},     {"plus", TOK_PLUS},
      {"minus", TOK_MINUS},     {"times", TOK_TIMES},
      {"divided", TOK_DIVIDED}, {"by", TOK_BY},
      {"mod", TOK_MOD},         {"delete", TOK_DELETE},
      {"try", TOK_TRY},         {"catch", TOK_CATCH},
      {"finally", TOK_FINALLY}, {"raise", TOK_RAISE},
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
 * @brief Process a single escape sequence character
 *
 * Converts escape sequence characters to their actual values.
 * Supports: \n, \t, \\, \", \', \r, \0
 *
 * @param escaped_char The character following the backslash
 * @param out_char Output parameter for the converted character
 * @return true if valid escape sequence, false otherwise
 */
static bool process_escape_sequence(char escaped_char, char *out_char) {
  switch (escaped_char) {
  case 'n':
    *out_char = '\n';
    return true;
  case 't':
    *out_char = '\t';
    return true;
  case '\\':
    *out_char = '\\';
    return true;
  case '"':
    *out_char = '"';
    return true;
  case '\'':
    *out_char = '\'';
    return true;
  case 'r':
    *out_char = '\r';
    return true;
  case '0':
    *out_char = '\0';
    return true;
  default:
    // Unknown escape sequence - return the character as-is
    *out_char = escaped_char;
    return true;
  }
}

/**
 * @brief Tokenize a single line of source code
 *
 * Processes one line, extracting all tokens. Handles:
 * - Numbers (integers and floats, with optional leading + or -)
 * - Strings and f-strings (with escape sequences)
 * - Multi-line strings with triple quotes (""" or ''')
 * - Keywords and identifiers
 * - Operators and punctuation
 * - Indentation tokens
 *
 * @param arr Token array to append tokens to
 * @param line The line to tokenize (should not include leading whitespace)
 * @param indent Indentation level in spaces (already calculated)
 * @param line_number 1-based line number for this line
 * @param full_source Complete source code (for multi-line string support)
 * @param source_pos Current position in full_source (updated when reading
 * multi-line strings)
 * @param source_len Total length of full_source
 * @param out_err Optional pointer to receive error information
 * @return true on success, false on error (e.g., unterminated string)
 */
static bool tokenize_line(TokenArray *arr, const char *line, int indent,
                          size_t line_number, const char *full_source,
                          size_t *source_pos, size_t source_len,
                          TokenizeError **out_err) {
  size_t len = strlen(line);
  size_t col = 0;

  // Add indent token if line is not empty
  // Column is 1-based, so indent token starts at column 1
  if (len > 0) {
    Token tok = {TOK_INDENT, NULL, 0, indent, line_number, 1};
    if (!token_array_add(arr, tok, out_err, line_number, 1))
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
      // Column is 1-based: indent (spaces) + position in line + 1
      size_t token_col = indent + start + 1;
      Token tok = {TOK_NUMBER, NULL, col - start, 0, line_number, token_col};
      char *text_buf = malloc(tok.length + 1);
      if (!text_buf) {
        tokenizer_report_error(out_err,
                               "Failed to allocate memory for number literal",
                               line_number, token_col);
        return false;
      }
      memcpy(text_buf, line + start, tok.length);
      text_buf[tok.length] = '\0';
      tok.text = text_buf;
      if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
        free(text_buf);
        return false;
      }
      continue;
    }

    // Check for f-string prefix (f"..." or f'...' or f"""...""" or f'''...''')
    // F-strings allow embedded expressions like f"Hello {name}"
    bool is_fstring = false;
    bool is_triple_quote = false;
    if (col + 1 < len && line[col] == 'f' &&
        (line[col + 1] == '"' || line[col + 1] == '\'')) {
      is_fstring = true;
      col++; // Skip 'f'
      // Check for triple quotes
      if (col + 2 < len && line[col] == line[col + 1] &&
          line[col + 1] == line[col + 2]) {
        is_triple_quote = true;
      }
    } else if (col + 2 < len && ((line[col] == '"' && line[col + 1] == '"' &&
                                  line[col + 2] == '"') ||
                                 (line[col] == '\'' && line[col + 1] == '\'' &&
                                  line[col + 2] == '\''))) {
      is_triple_quote = true;
    }

    // Tokenize string literals (handles escape sequences and multi-line
    // strings) Supports both single and double quotes, and triple-quoted
    // multi-line strings
    if (line[col] == '"' || line[col] == '\'') {
      char quote_char = line[col];
      size_t quote_count = is_triple_quote ? 3 : 1;

      if (is_triple_quote) {
        // Multi-line string: read from full source across line boundaries
        // Calculate absolute position in full source
        size_t abs_pos = *source_pos + col;
        size_t start_pos = abs_pos + quote_count; // After opening quotes
        size_t pos = start_pos;
        bool closed = false;
        size_t escape_count = 0;

        // Read forward until we find closing triple quotes
        while (pos + 2 < source_len) {
          if (full_source[pos] == '\\') {
            // Handle escape sequences
            if (pos + 1 < source_len) {
              escape_count++;
              pos += 2; // Skip backslash and escaped char
              continue;
            } else {
              break;
            }
          } else if (full_source[pos] == quote_char &&
                     full_source[pos + 1] == quote_char &&
                     full_source[pos + 2] == quote_char) {
            closed = true;
            break;
          }
          pos++;
        }

        if (!closed) {
          size_t token_col = indent + col + 1;
          tokenizer_report_error(out_err,
                                 "Unterminated multi-line string literal",
                                 line_number, token_col);
          return false;
        }

        // Calculate content length (from start_pos to pos, excluding closing
        // quotes)
        size_t content_len = pos - start_pos;
        size_t actual_len = content_len - escape_count; // Escapes become 1 char

        size_t token_col = indent + (is_fstring ? col - 1 : col) + 1;
        Token tok = {is_fstring ? TOK_FSTRING : TOK_STRING,
                     NULL,
                     actual_len,
                     0,
                     line_number,
                     token_col};
        char *text_buf = malloc(actual_len + 1);
        if (!text_buf) {
          tokenizer_report_error(out_err,
                                 "Failed to allocate memory for string literal",
                                 line_number, token_col);
          return false;
        }

        // Copy content and process escape sequences
        size_t dest_pos = 0;
        pos = start_pos;
        while (pos < start_pos + content_len) {
          if (full_source[pos] == '\\' && pos + 1 < start_pos + content_len) {
            char converted_char;
            process_escape_sequence(full_source[pos + 1], &converted_char);
            text_buf[dest_pos++] = converted_char;
            pos += 2;
          } else {
            text_buf[dest_pos++] = full_source[pos++];
          }
        }
        text_buf[actual_len] = '\0';
        tok.text = text_buf;
        if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
          free(text_buf);
          return false;
        }

        // Update source_pos to point after closing triple quotes
        *source_pos = pos + quote_count;
        // Return false to signal caller to skip to the new position
        // (The caller will need to handle this)
        return true;
      } else {
        // Single-line string: original logic
        size_t content_start = col + quote_count;
        size_t cursor = content_start;
        bool closed = false;
        size_t escape_count = 0;

        while (cursor < len) {
          if (line[cursor] == '\\') {
            cursor++;
            if (cursor < len) {
              escape_count++; // Escape sequence takes 2 chars but becomes 1
                              // char
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
          size_t token_col = indent + col + 1;
          tokenizer_report_error(out_err, "Unterminated string literal",
                                 line_number, token_col);
          return false;
        }

        size_t source_len = cursor - content_start;
        size_t actual_len = source_len - escape_count;

        size_t token_start_col = is_fstring ? col - 1 : col;
        size_t token_col = indent + token_start_col + 1;
        Token tok = {is_fstring ? TOK_FSTRING : TOK_STRING,
                     NULL,
                     actual_len,
                     0,
                     line_number,
                     token_col};
        char *text_buf = malloc(actual_len + 1);
        if (!text_buf) {
          tokenizer_report_error(out_err,
                                 "Failed to allocate memory for string literal",
                                 line_number, token_col);
          return false;
        }

        // Copy content and process escape sequences
        size_t dest_pos = 0;
        cursor = content_start;
        while (cursor < content_start + source_len) {
          if (line[cursor] == '\\' && cursor + 1 < content_start + source_len) {
            char converted_char;
            process_escape_sequence(line[cursor + 1], &converted_char);
            text_buf[dest_pos++] = converted_char;
            cursor += 2;
          } else {
            text_buf[dest_pos++] = line[cursor++];
          }
        }
        text_buf[actual_len] = '\0';
        tok.text = text_buf;
        if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
          free(text_buf);
          return false;
        }
        col = cursor + quote_count; // Skip closing quote(s)
        continue;
      }
    }

    // Tokenize identifiers and keywords
    // Identifiers can contain letters, digits, underscores, and dots
    // (dots are allowed for module.function syntax like math.sqrt)
    // Supports UTF-8 Unicode characters in identifiers
    if (can_start_identifier(line, col, len)) {
      size_t start = col;
      // Advance past the first character (may be multi-byte UTF-8)
      unsigned char first_byte = (unsigned char)line[col];
      if (first_byte < 0x80) {
        col++; // ASCII
      } else {
        size_t seq_len = utf8_sequence_length(first_byte);
        if (seq_len > 0 && col + seq_len <= len) {
          col += seq_len;
        } else {
          col++; // Fallback: skip invalid byte
        }
      }
      // Continue reading identifier characters
      while (col < len && can_continue_identifier(line, col, len)) {
        unsigned char byte = (unsigned char)line[col];
        if (byte < 0x80) {
          col++; // ASCII
        } else {
          size_t seq_len = utf8_sequence_length(byte);
          if (seq_len > 0 && col + seq_len <= len) {
            col += seq_len;
          } else {
            col++; // Fallback: skip invalid byte
          }
        }
      }
      size_t word_len = col - start;
      TokenType type = match_keyword(line + start, word_len);
      size_t token_col = indent + start + 1;
      Token tok = {type, NULL, word_len, 0, line_number, token_col};
      char *text_buf = malloc(tok.length + 1);
      if (!text_buf) {
        tokenizer_report_error(out_err,
                               "Failed to allocate memory for identifier token",
                               line_number, token_col);
        return false;
      }
      strncpy(text_buf, line + start, tok.length);
      text_buf[tok.length] = '\0';
      tok.text = text_buf;
      if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
        free(text_buf);
        return false;
      }
      continue;
    }

    // Tokenize single-character punctuation
    // Colon is used for if/for/while statement headers
    if (line[col] == ':') {
      size_t token_col = indent + col + 1;
      Token tok = {TOK_COLON, NULL, 1, 0, line_number, token_col};
      tok.text = strdup(":");
      if (!tok.text) {
        tokenizer_report_error(out_err, "Failed to allocate colon token",
                               line_number, token_col);
        return false;
      }
      if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
        free((void *)tok.text);
        return false;
      }
      col++;
      continue;
    }

    if (line[col] == ',') {
      size_t token_col = indent + col + 1;
      Token tok = {TOK_COMMA, NULL, 1, 0, line_number, token_col};
      tok.text = strdup(",");
      if (!tok.text) {
        tokenizer_report_error(out_err, "Failed to allocate comma token",
                               line_number, token_col);
        return false;
      }
      if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
        free((void *)tok.text);
        return false;
      }
      col++;
      continue;
    }

    // Handle '-' as operator token when not part of a number
    // (for unary negation support)
    if (line[col] == '-') {
      size_t token_col = indent + col + 1;
      Token tok = {TOK_MINUS, NULL, 1, 0, line_number, token_col};
      tok.text = strdup("minus");
      if (!tok.text) {
        tokenizer_report_error(out_err, "Failed to allocate minus token",
                               line_number, token_col);
        return false;
      }
      if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
        free((void *)tok.text);
        return false;
      }
      col++;
      continue;
    }

    // Unknown character - report error
    size_t token_col = indent + col + 1;
    tokenizer_report_error(out_err, "Unknown character encountered",
                           line_number, token_col);
    return false;
  }

  // Add newline token to mark end of line (if line had content)
  // Empty lines don't get newline tokens to avoid clutter
  if (len > 0) {
    char *newline_text = strdup("\n");
    if (!newline_text) {
      size_t token_col = indent + len + 1;
      tokenizer_report_error(out_err, "Failed to allocate newline token text",
                             line_number, token_col);
      return false;
    }
    // Newline is at the end of the line
    size_t token_col = indent + len + 1;
    Token tok = {TOK_NEWLINE, newline_text, 1, 0, line_number, token_col};
    if (!token_array_add(arr, tok, out_err, line_number, token_col)) {
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
 * @brief Tokenize Kronos source code with configurable tab width
 *
 * Main entry point for lexical analysis. Splits source into lines, calculates
 * indentation, and tokenizes each line. Handles mixed indentation errors
 * (spaces and tabs in the same block).
 *
 * @param source Complete source code to tokenize (must not be NULL)
 * @param out_err Optional pointer to receive error information
 * @param tab_width Tab width in spaces (default: 8). Must be > 0.
 *                  If 0 is passed, defaults to 8.
 * @return Token array on success, NULL on error
 */
TokenArray *tokenize_with_tab_width(const char *source, TokenizeError **out_err,
                                    int tab_width) {
  // Use default tab width if invalid value provided
  if (tab_width <= 0) {
    tab_width = 8;
  }
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

  size_t source_len = strlen(source);
  // Process source line by line
  const char *line_start = source;
  const char *line_end;
  size_t line_number = 1;
  size_t source_pos =
      0; // Track absolute position in source for multi-line strings

  while (*line_start && source_pos < source_len) {
    // Find the end of the current line (newline or end of string)
    line_end = line_start;
    while (*line_end && *line_end != '\n') {
      line_end++;
    }

    // Calculate line length
    size_t line_len = line_end - line_start;

    // Calculate indentation level
    // Tabs are treated as TOKENIZER_TAB_WIDTH spaces
    // Mixed spaces and tabs in the same line is reported as an error but
    // tokenization continues (recovery mode)
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
        indent += tab_width;
      } else {
        break; // End of leading whitespace
      }

      // Check for mixed indentation
      // Report error but continue processing (recovery mode)
      if (saw_space && saw_tab) {
        // Report error but don't abort - allow recovery
        // Use the indentation calculated so far
        tokenizer_report_error(out_err,
                               "Mixed indentation (spaces and tabs detected "
                               "on the same line)",
                               line_number, 1);
        // Continue processing with the indentation calculated so far
        break;
      }
    }

    // Extract line content (after leading whitespace)
    const char *content_start = line_start + i;
    size_t content_len = line_len - i;

    if (content_len > 0) {
      char *line = malloc(content_len + 1);
      if (!line) {
        tokenizer_report_error(out_err,
                               "Failed to allocate memory while tokenizing",
                               line_number, 1);
        token_array_free(arr);
        return NULL;
      }
      strncpy(line, content_start, content_len);
      line[content_len] = '\0';

      size_t old_source_pos = source_pos;
      if (!tokenize_line(arr, line, indent, line_number, source, &source_pos,
                         source_len, out_err)) {
        if (!*out_err) {
          tokenizer_report_error(out_err,
                                 "Failed to tokenize line (out of memory)",
                                 line_number, 1);
        }
        free(line);
        token_array_free(arr);
        return NULL;
      }
      free(line);

      // If source_pos advanced beyond current line, skip to that position
      // (multi-line string consumed multiple lines)
      if (source_pos > old_source_pos + line_len) {
        // Find the line that source_pos points to
        line_start = source + source_pos;
        // Find the start of the current line
        while (line_start > source && line_start[-1] != '\n') {
          line_start--;
        }
        // Count lines to update line_number
        const char *p = source;
        line_number = 1;
        while (p < line_start) {
          if (*p == '\n') {
            line_number++;
          }
          p++;
        }
        continue; // Process the new line
      }
    }

    // Move to next line
    source_pos = line_end - source;
    if (*line_end == '\n') {
      source_pos++; // Skip newline
    }
    line_start = line_end;
    if (*line_start == '\n')
      line_start++;
    line_number++;
  }

  // Add end-of-file token to mark completion
  // EOF is at the end of the last line (or line 1 if file is empty)
  size_t eof_line = line_number > 1 ? line_number - 1 : 1;
  Token eof = {TOK_EOF, NULL, 0, 0, eof_line, 1};
  if (!token_array_add(arr, eof, out_err, eof_line, 1)) {
    if (!*out_err) {
      tokenizer_report_error(
          out_err, "Failed to append EOF token (out of memory)", eof_line, 1);
    }
    token_array_free(arr);
    return NULL;
  }

  return arr;
}

/**
 * @brief Tokenize Kronos source code (default tab width)
 *
 * Wrapper around tokenize_with_tab_width() that uses the default tab width
 * of 8. This maintains backward compatibility with existing code.
 *
 * @param source Complete source code to tokenize (must not be NULL)
 * @param out_err Optional pointer to receive error information
 * @return Token array on success, NULL on error
 */
TokenArray *tokenize(const char *source, TokenizeError **out_err) {
  return tokenize_with_tab_width(source, out_err, 8);
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
