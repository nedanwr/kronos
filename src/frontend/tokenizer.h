#ifndef KRONOS_TOKENIZER_H
#define KRONOS_TOKENIZER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Token types matching the Python implementation
typedef enum {
  TOK_NUMBER,
  TOK_STRING,
  TOK_SET,
  TOK_LET,
  TOK_TO,
  TOK_AS,
  TOK_IF,
  TOK_FOR,
  TOK_WHILE,
  TOK_IN,
  TOK_RANGE,
  TOK_FUNCTION,
  TOK_WITH,
  TOK_CALL,
  TOK_RETURN,
  TOK_TRUE,
  TOK_FALSE,
  TOK_NULL,
  TOK_IS,
  TOK_EQUAL,
  TOK_NOT,
  TOK_GREATER,
  TOK_LESS,
  TOK_THAN,
  TOK_AND,
  TOK_OR,
  TOK_PRINT,
  TOK_PLUS,
  TOK_MINUS,
  TOK_TIMES,
  TOK_DIVIDED,
  TOK_BY,
  TOK_NAME,
  TOK_COLON,
  TOK_COMMA,
  TOK_NEWLINE,
  TOK_INDENT,
  TOK_EOF,
} TokenType;

typedef struct {
  TokenType type;
  const char
      *text; // Heap-allocated, nul-terminated string owned by this Token.
             // Allocated via malloc()/strdup() during tokenization.
             // Must be freed with token_free() or free((char *)text) when
             // the Token is no longer part of a TokenArray. If the Token
             // is part of a TokenArray, token_array_free() will free it
             // automatically. The pointer becomes invalid after freeing.
  size_t length;
  int indent_level; // For INDENT tokens
} Token;

typedef struct {
  Token *tokens;
  size_t count;
  size_t capacity;
} TokenArray;

// Error information for tokenization failures
typedef struct {
  char *message; // Error message (heap-allocated, owned by TokenizeError)
  size_t line;   // 1-based line number where error occurred (0 if unknown)
  size_t column; // 1-based column number where error occurred (0 if unknown)
} TokenizeError;

// Tokenize source code
// @param source Source code to tokenize (must not be NULL).
// @param out_err Optional pointer to receive error details on failure.
//                If non-NULL and an error occurs, *out_err is set to a
//                heap-allocated TokenizeError (caller must free with
//                tokenize_error_free()). On success, *out_err is set to NULL.
// @return TokenArray* on success, NULL on error (allocation failure or
//         invalid input). On error, if out_err is non-NULL, *out_err contains
//         error details. Caller must free the returned TokenArray with
//         token_array_free() and any TokenizeError with tokenize_error_free().
TokenArray *tokenize(const char *source, TokenizeError **out_err);

// Free a TokenizeError structure
// @param err TokenizeError to free (may be NULL, in which case this is a
// no-op).
void tokenize_error_free(TokenizeError *err);

// Free a single Token's resources (frees the text string)
// Use this when managing Tokens outside of a TokenArray.
// If the Token is part of a TokenArray, use token_array_free() instead.
void token_free(Token *token);

// Free a TokenArray and all its Tokens
// Automatically frees all Token.text strings in the array.
void token_array_free(TokenArray *array);

// Debug
void token_print(Token *token);

#ifdef __cplusplus
}
#endif

#endif // KRONOS_TOKENIZER_H
