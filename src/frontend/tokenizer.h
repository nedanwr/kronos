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
  TOK_FSTRING,
  TOK_SET,
  TOK_LET,
  TOK_TO,
  TOK_AS,
  TOK_IF,
  TOK_ELSE,
  TOK_ELSE_IF,
  TOK_FOR,
  TOK_WHILE,
  TOK_BREAK,
  TOK_CONTINUE,
  TOK_IN,
  TOK_RANGE,
  TOK_LIST,
  TOK_MAP,
  TOK_AT,
  TOK_FROM,
  TOK_END,
  TOK_FUNCTION,
  TOK_WITH,
  TOK_CALL,
  TOK_RETURN,
  TOK_IMPORT,
  TOK_TRUE,
  TOK_FALSE,
  TOK_NULL,
  TOK_UNDEFINED,
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
  TOK_MOD,
  TOK_DELETE,
  TOK_TRY,
  TOK_CATCH,
  TOK_FINALLY,
  TOK_RAISE,
  TOK_NAME,
  TOK_COLON,
  TOK_COMMA,
  TOK_NEWLINE,
  TOK_INDENT,
  TOK_EOF,
} TokenType;

typedef struct {
  TokenType type;
  const char *
      text; // Token text string (nul-terminated).
            //
            // OWNERSHIP RULES:
            // - If Token is part of a TokenArray: TokenArray owns the text.
            //   Use token_array_free() to free the entire array and all tokens.
            // - If Token is extracted/copied from a TokenArray: Caller owns the
            // text.
            //   Use token_free() or free((char *)text) to free it.
            // - Static constants (colon, comma, minus, newline): Never freed.
            //   These are static strings that don't require freeing.
            //
            // Allocated via malloc()/strdup() during tokenization for dynamic
            // tokens. For static tokens (TOK_COLON, TOK_COMMA, TOK_MINUS,
            // TOK_NEWLINE), points to static string constants that must not be
            // freed. The pointer becomes invalid after freeing (for
            // heap-allocated text).
  size_t length;
  int indent_level; // For INDENT tokens
  size_t line;   // 1-based line number where this token starts (0 if unknown)
  size_t column; // 1-based column number where this token starts (0 if unknown)
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

// Tokenize source code with configurable tab width
// @param source Source code to tokenize (must not be NULL).
// @param out_err Optional pointer to receive error details on failure.
//                If non-NULL and an error occurs, *out_err is set to a
//                heap-allocated TokenizeError (caller must free with
//                tokenize_error_free()). On success, *out_err is set to NULL.
// @param tab_width Tab width in spaces (default: 8). Must be > 0.
//                  If 0 is passed, defaults to 8.
// @return TokenArray* on success, NULL on error (allocation failure or
//         invalid input). On error, if out_err is non-NULL, *out_err contains
//         error details. Caller must free the returned TokenArray with
//         token_array_free() and any TokenizeError with tokenize_error_free().
TokenArray *tokenize_with_tab_width(const char *source, TokenizeError **out_err,
                                    int tab_width);

// Tokenize source code (default tab width of 8)
// Wrapper around tokenize_with_tab_width() for backward compatibility.
// @param source Source code to tokenize (must not be NULL).
// @param out_err Optional pointer to receive error details on failure.
//                If non-NULL and an error occurs, *out_err is set to a
//                heap-allocated TokenizeError (caller must free with
//                tokenize_error_free()). On success, *out_err is set to NULL.
// @return TokenArray* on success, NULL on error (allocation failure or
//         invalid input). On error, if out_err is non-NULL, *out_err contains
//         error details.
//
// OWNERSHIP: Caller owns the returned TokenArray and must free it with
//            token_array_free(). The TokenArray owns all Token.text strings.
//            If you extract Tokens from the array, you own those tokens and
//            must free them with token_free().
//
// Also free any TokenizeError with tokenize_error_free().
TokenArray *tokenize(const char *source, TokenizeError **out_err);

// Free a TokenizeError structure
// @param err TokenizeError to free (may be NULL, in which case this is a
// no-op).
void tokenize_error_free(TokenizeError *err);

// Free a single Token's resources (frees the text string)
//
// OWNERSHIP: Use this when you own a Token that is NOT part of a TokenArray.
// Examples:
//   - Token extracted/copied from a TokenArray (caller now owns it)
//   - Token created manually outside of tokenization
//
// DO NOT use this for Tokens that are still part of a TokenArray.
// Instead, use token_array_free() to free the entire array.
//
// SAFETY: This function safely handles static string constants (colon, comma,
//         minus, newline) and will not attempt to free them.
// Note: token_array_free() uses this function internally for consistency.
void token_free(Token *token);

// Free a TokenArray and all its Tokens
//
// OWNERSHIP: Frees the TokenArray structure and all Token.text strings it
// contains. After calling this, the TokenArray pointer and all Token pointers
// within it become invalid and must not be used.
//
// SAFETY: Automatically handles static string constants (colon, comma, minus,
//         newline) and will not attempt to free them.
//
// If you need to extract Tokens from the array before freeing, copy them first
// (they will own their text), then free the array.
void token_array_free(TokenArray *array);

// Debug
void token_print(Token *token);

#ifdef __cplusplus
}
#endif

#endif // KRONOS_TOKENIZER_H
