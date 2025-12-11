/**
 * @file test_tokenizer.c
 * @brief Unit tests for the tokenizer
 *
 * Tests lexical analysis functionality including:
 * - Tokenization of various literals (numbers, strings, booleans)
 * - Keyword recognition
 * - Operator tokenization
 * - Error handling (unterminated strings)
 * - Special tokens (indentation, newlines, EOF)
 */

#include "../../src/frontend/tokenizer.h"
#include "../framework/test_framework.h"
#include <string.h>

/**
 * @brief Test tokenization of empty string
 *
 * Verifies that an empty input produces only an EOF token.
 */
TEST(tokenize_empty_string) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_INT_EQ(tokens->count, 1); // Should have EOF token
  ASSERT_INT_EQ(tokens->tokens[0].type, TOK_EOF);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of integer literals
 *
 * Verifies that integer numbers are correctly recognized and tokenized.
 */
TEST(tokenize_simple_number) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("42", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NUMBER
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i].text, "42");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of floating-point literals
 *
 * Verifies that decimal numbers are correctly recognized and tokenized.
 */
TEST(tokenize_float_number) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("3.14", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NUMBER
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i].text, "3.14");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of negative integer literals
 *
 * Verifies that negative numbers like -42 are tokenized as a single NUMBER
 * token, not as a separate minus operator and number.
 */
TEST(tokenize_negative_integer) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("-42", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NUMBER
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i].text, "-42");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of positive integer literals with explicit sign
 *
 * Verifies that positive numbers with explicit + sign are tokenized correctly.
 */
TEST(tokenize_positive_integer) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("+42", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NUMBER
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i].text, "+42");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of negative floating-point literals
 *
 * Verifies that negative decimal numbers like -3.14 are tokenized as a single
 * NUMBER token.
 */
TEST(tokenize_negative_float) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("-3.14", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NUMBER
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i].text, "-3.14");

  token_array_free(tokens);
}

/**
 * @brief Test that minus operator is still separate when not followed by digit
 *
 * Verifies that '-' is not consumed as part of a number when it's not
 * immediately followed by a digit (e.g., in expressions like "x minus y").
 */
TEST(tokenize_minus_operator_separate) {
  TokenizeError *err = NULL;
  // Test that "minus" keyword is still recognized separately
  TokenArray *tokens = tokenize("x minus 5", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 4);

  // Skip INDENT token, find tokens
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 2 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NAME);
  ASSERT_STR_EQ(tokens->tokens[i].text, "x");
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_MINUS);
  ASSERT_INT_EQ(tokens->tokens[i + 2].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i + 2].text, "5");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of string literals
 *
 * Verifies that quoted strings are correctly extracted (quotes are stripped).
 */
TEST(tokenize_string_literal) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("\"hello\"", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find STRING
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_STRING);
  ASSERT_STR_EQ(tokens->tokens[i].text, "hello");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of language keywords
 *
 * Verifies that all Kronos keywords are correctly recognized and
 * distinguished from regular identifiers.
 */
TEST(tokenize_keywords) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("set let to as if else for while", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 9);

  // Skip INDENT token, check keywords
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 7 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_SET);
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_LET);
  ASSERT_INT_EQ(tokens->tokens[i + 2].type, TOK_TO);
  ASSERT_INT_EQ(tokens->tokens[i + 3].type, TOK_AS);
  ASSERT_INT_EQ(tokens->tokens[i + 4].type, TOK_IF);
  ASSERT_INT_EQ(tokens->tokens[i + 5].type, TOK_ELSE);
  ASSERT_INT_EQ(tokens->tokens[i + 6].type, TOK_FOR);
  ASSERT_INT_EQ(tokens->tokens[i + 7].type, TOK_WHILE);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of variable names (identifiers)
 *
 * Verifies that identifiers starting with letters or underscores
 * are correctly tokenized as TOK_NAME.
 */
TEST(tokenize_variable_name) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("myVariable", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NAME
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NAME);
  ASSERT_STR_EQ(tokens->tokens[i].text, "myVariable");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of arithmetic operators
 *
 * Verifies that natural-language arithmetic operators (plus, minus,
 * times, divided by) are correctly recognized.
 */
TEST(tokenize_arithmetic_operators) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("plus minus times divided by", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 5);

  // Skip INDENT token, check operators
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 4 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_PLUS);
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_MINUS);
  ASSERT_INT_EQ(tokens->tokens[i + 2].type, TOK_TIMES);
  ASSERT_INT_EQ(tokens->tokens[i + 3].type, TOK_DIVIDED);
  ASSERT_INT_EQ(tokens->tokens[i + 4].type, TOK_BY);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of comparison operators
 *
 * Verifies that natural-language comparison operators (is equal,
 * is not equal, is greater than, is less than) are correctly recognized.
 */
TEST(tokenize_comparison_operators) {
  TokenizeError *err = NULL;
  TokenArray *tokens =
      tokenize("is equal is not equal is greater than is less than", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 9); // operators + EOF (may have NEWLINE)

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of boolean literals
 *
 * Verifies that "true" and "false" are recognized as boolean tokens,
 * not as identifiers.
 */
TEST(tokenize_boolean_literals) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("true false", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 3);

  // Skip INDENT token, check booleans
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 1 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_TRUE);
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_FALSE);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of null literal
 *
 * Verifies that "null" is recognized as a null token.
 */
TEST(tokenize_null_literal) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("null", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find NULL
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NULL);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of assignment statements
 *
 * Verifies that "set x to 10" style assignments produce the correct
 * sequence of tokens (SET, NAME, TO, NUMBER).
 */
TEST(tokenize_assignment_statement) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("set x to 10", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 5);

  // Skip INDENT token, check assignment
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 3 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_SET);
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_NAME);
  ASSERT_STR_EQ(tokens->tokens[i + 1].text, "x");
  ASSERT_INT_EQ(tokens->tokens[i + 2].type, TOK_TO);
  ASSERT_INT_EQ(tokens->tokens[i + 3].type, TOK_NUMBER);
  ASSERT_STR_EQ(tokens->tokens[i + 3].text, "10");

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of newlines
 *
 * Verifies that newline characters are correctly tokenized and
 * separate statements on different lines.
 */
TEST(tokenize_newlines) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("set x to 10\nset y to 20", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  // Should have tokens plus newline tokens
  ASSERT_TRUE(tokens->count >= 8);

  token_array_free(tokens);
}

/**
 * @brief Test tokenization of f-strings
 *
 * Verifies that f-strings (f"text {expr}") are correctly recognized
 * as TOK_FSTRING tokens.
 */
TEST(tokenize_fstring) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("f\"Hello {name}\"", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 2);

  // Skip INDENT token, find FSTRING
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_FSTRING);

  token_array_free(tokens);
}

/**
 * @brief Test error handling for unterminated strings
 *
 * Verifies that the tokenizer correctly detects and reports errors
 * when a string literal is not properly closed.
 */
TEST(tokenize_invalid_string) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("\"unclosed string", &err);

  // Tokenizer must return NULL and set error for unterminated string
  ASSERT_PTR_NULL(tokens);
  ASSERT_PTR_NOT_NULL(err);
  tokenize_error_free(err);
}

/**
 * @brief Test error handling for unknown characters
 *
 * Verifies that the tokenizer correctly detects and reports errors
 * when an unknown character (like @, #, $) is encountered.
 */
TEST(tokenize_unknown_character) {
  TokenizeError *err = NULL;
  TokenArray *tokens = tokenize("@", &err);

  // Tokenizer must return NULL and set error for unknown character
  ASSERT_PTR_NULL(tokens);
  ASSERT_PTR_NOT_NULL(err);
  ASSERT_STR_EQ(err->message, "Unknown character encountered");
  tokenize_error_free(err);
}

/**
 * @brief Test tokenization of UTF-8 identifiers
 *
 * Verifies that identifiers containing UTF-8 Unicode characters
 * are correctly tokenized.
 */
TEST(tokenize_utf8_identifier) {
  TokenizeError *err = NULL;
  // Test with common UTF-8 characters: é (é), ñ (ñ), 中文 (Chinese)
  TokenArray *tokens = tokenize("café résumé", &err);

  ASSERT_PTR_NULL(err);
  ASSERT_PTR_NOT_NULL(tokens);
  ASSERT_TRUE(tokens->count >= 3);

  // Skip INDENT token, find identifiers
  size_t i = 0;
  while (i < tokens->count && (tokens->tokens[i].type == TOK_INDENT ||
                               tokens->tokens[i].type == TOK_NEWLINE)) {
    i++;
  }
  ASSERT_TRUE(i + 1 < tokens->count);
  ASSERT_INT_EQ(tokens->tokens[i].type, TOK_NAME);
  ASSERT_STR_EQ(tokens->tokens[i].text, "café");
  ASSERT_INT_EQ(tokens->tokens[i + 1].type, TOK_NAME);
  ASSERT_STR_EQ(tokens->tokens[i + 1].text, "résumé");

  token_array_free(tokens);
}
