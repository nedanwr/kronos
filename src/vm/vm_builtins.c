#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "vm_builtins.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#endif

#if (defined(HAVE_POSIX_REGEX) && HAVE_POSIX_REGEX) || !defined(_WIN32)
#include <regex.h>
#define KRONOS_HAS_POSIX_REGEX 1
#else
#define KRONOS_HAS_POSIX_REGEX 0
#endif

#define NUMBER_STRING_BUFFER_SIZE 64
#define REGEX_ERROR_BUFFER_SIZE 256
#define PORTABLE_GETLINE_INITIAL_SIZE 256

static FILE *portable_fopen(const char *path, const char *mode) {
#ifdef _WIN32
  int path_len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (path_len <= 0) {
    return NULL;
  }
  wchar_t *wpath = malloc(path_len * sizeof(wchar_t));
  if (!wpath) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_len) <= 0) {
    free(wpath);
    return NULL;
  }

  int mode_len = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
  if (mode_len <= 0) {
    free(wpath);
    return NULL;
  }
  wchar_t *wmode = malloc(mode_len * sizeof(wchar_t));
  if (!wmode) {
    free(wpath);
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mode_len) <= 0) {
    free(wpath);
    free(wmode);
    return NULL;
  }

  FILE *file = _wfopen(wpath, wmode);
  free(wpath);
  free(wmode);
  return file;
#else
  return fopen(path, mode);
#endif
}

static ssize_t __attribute__((unused))
portable_getline(char **lineptr, size_t *n, FILE *stream) {
  if (!lineptr || !n || !stream) {
    return -1;
  }

  if (!*lineptr || *n == 0) {
    *n = PORTABLE_GETLINE_INITIAL_SIZE;
    *lineptr = malloc(*n);
    if (!*lineptr) {
      return -1;
    }
  }

  size_t pos = 0;
  int c;

  while ((c = fgetc(stream)) != EOF) {
    if (pos + 1 >= *n) {
      size_t new_size = *n * 2;
      char *new_buf = realloc(*lineptr, new_size);
      if (!new_buf) {
        return -1;
      }
      *lineptr = new_buf;
      *n = new_size;
    }

    (*lineptr)[pos++] = (char)c;
    if (c == '\n') {
      break;
    }
  }

  if (pos == 0 && c == EOF) {
    return -1;
  }

  (*lineptr)[pos] = '\0';
  return (ssize_t)pos;
}

#ifdef _WIN32
#define KRONOS_GETLINE(lineptr, n, stream) portable_getline(lineptr, n, stream)
#else
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200809L
#define KRONOS_GETLINE(lineptr, n, stream) getline(lineptr, n, stream)
#else
#define KRONOS_GETLINE(lineptr, n, stream) portable_getline(lineptr, n, stream)
#endif
#endif

static int sort_compare_values(const void *a, const void *b) {
  const KronosValue *val_a = *(const KronosValue *const *)a;
  const KronosValue *val_b = *(const KronosValue *const *)b;

  if (val_a->type == VAL_NUMBER) {
    double diff = val_a->as.number - val_b->as.number;
    return (diff > 0) - (diff < 0);
  }
  if (val_a->type == VAL_STRING) {
    return strcmp(val_a->as.string.data, val_b->as.string.data);
  }
  return 0;
}

static void cleanup_call_frame_locals(CallFrame *frame) {
  for (size_t i = 0; i < frame->local_count; i++) {
    if (frame->locals[i].name) {
      free(frame->locals[i].name);
      frame->locals[i].name = NULL;
    }
    if (frame->locals[i].value) {
      value_release(frame->locals[i].value);
      frame->locals[i].value = NULL;
    }
    if (frame->locals[i].type_name) {
      free(frame->locals[i].type_name);
      frame->locals[i].type_name = NULL;
    }
  }
  frame->local_count = 0;

  for (size_t i = 0; i < LOCALS_MAX; i++) {
    frame->local_hash[i] = NULL;
  }
}

static int vm_propagate_error(KronosVM *vm, KronosErrorCode fallback) {
  KronosErrorCode code =
      (vm && vm->last_error_code != KRONOS_OK) ? vm->last_error_code : fallback;
  return code == KRONOS_OK ? -(int)fallback : -(int)code;
}

static int push(KronosVM *vm, KronosValue *value) {
  if (vm->stack_top >= vm->stack + STACK_MAX) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Stack overflow (too many nested operations or calls)");
  }
  *vm->stack_top = value;
  vm->stack_top++;
  value_retain(value);
  return 0;
}

static KronosValue *pop(KronosVM *vm) {
  if (vm->stack_top <= vm->stack) {
    vm_set_error(vm, KRONOS_ERR_RUNTIME,
                 "Stack underflow (internal error - please report this bug)");
    return NULL;
  }
  vm->stack_top--;
  return *vm->stack_top;
}

#define POP_OR_RETURN(vm, var)                                                 \
  do {                                                                         \
    (var) = pop(vm);                                                           \
    if (!(var)) {                                                              \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                      \
    }                                                                          \
  } while (0)

#define POP_OR_RETURN_WITH_CLEANUP(vm, var, cleanup)                           \
  do {                                                                         \
    (var) = pop(vm);                                                           \
    if (!(var)) {                                                              \
      cleanup;                                                                 \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                      \
    }                                                                          \
  } while (0)

#define PUSH_OR_RETURN_WITH_CLEANUP(vm, value, cleanup)                        \
  do {                                                                         \
    if (push(vm, value) != 0) {                                                \
      cleanup;                                                                 \
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);                      \
    }                                                                          \
  } while (0)

static int is_path_separator(char c) { return c == '/' || c == '\\'; }

static int run_function_callback(KronosVM *vm, const char *callback_name,
                                 KronosValue *callback, KronosValue **args,
                                 uint8_t arg_count,
                                 KronosValue **out_result) {
  if (!vm) {
    return -(int)KRONOS_ERR_INVALID_ARGUMENT;
  }
  if (!callback || !out_result) {
    return vm_error(vm, KRONOS_ERR_INVALID_ARGUMENT,
                    "Invalid callback invocation state");
  }

  Bytecode *saved_bytecode = vm->bytecode;
  uint8_t *saved_ip = vm->ip;
  size_t saved_call_stack_size = vm->call_stack_size;
  KronosValue **saved_stack_top = vm->stack_top;

  for (uint8_t i = 0; i < arg_count; i++) {
    int push_status = push(vm, args[i]);
    if (push_status != 0) {
      while (vm->stack_top > saved_stack_top) {
        KronosValue *cleanup_val = pop(vm);
        if (!cleanup_val) {
          break;
        }
        value_release(cleanup_val);
      }
      return push_status;
    }
  }

  int call_status =
      vm_call_function_value(vm, callback,
                             callback_name ? callback_name : "callback",
                             arg_count);
  if (call_status != 0) {
    while (vm->stack_top > saved_stack_top) {
      KronosValue *cleanup_val = pop(vm);
      if (!cleanup_val) {
        break;
      }
      value_release(cleanup_val);
    }
    vm->bytecode = saved_bytecode;
    vm->ip = saved_ip;
    return call_status;
  }

  if (vm->call_stack_size <= saved_call_stack_size) {
    vm->bytecode = saved_bytecode;
    vm->ip = saved_ip;
    return vm_error(vm, KRONOS_ERR_INTERNAL,
                    "Callback call frame was not created");
  }

  CallFrame *callback_frame = &vm->call_stack[vm->call_stack_size - 1];
  callback_frame->return_ip = NULL;
  callback_frame->return_bytecode = NULL;

  int exec_status = vm_execute(vm, vm->bytecode);
  if (exec_status != 0) {
    if (vm->call_stack_size > saved_call_stack_size) {
      callback_frame = &vm->call_stack[vm->call_stack_size - 1];
      cleanup_call_frame_locals(callback_frame);
      if (callback_frame->owned_bytecode) {
        free(callback_frame->owned_bytecode);
        callback_frame->owned_bytecode = NULL;
      }
      vm->call_stack_size--;
    }
    while (vm->stack_top > saved_stack_top) {
      KronosValue *cleanup_val = pop(vm);
      if (!cleanup_val) {
        break;
      }
      value_release(cleanup_val);
    }
    vm->current_frame = saved_call_stack_size > 0
                            ? &vm->call_stack[saved_call_stack_size - 1]
                            : NULL;
    vm->bytecode = saved_bytecode;
    vm->ip = saved_ip;
    return exec_status;
  }

  KronosValue *result = pop(vm);
  if (!result) {
    if (vm->call_stack_size > saved_call_stack_size) {
      callback_frame = &vm->call_stack[vm->call_stack_size - 1];
      cleanup_call_frame_locals(callback_frame);
      if (callback_frame->owned_bytecode) {
        free(callback_frame->owned_bytecode);
        callback_frame->owned_bytecode = NULL;
      }
      vm->call_stack_size--;
    }
    while (vm->stack_top > saved_stack_top) {
      KronosValue *cleanup_val = pop(vm);
      if (!cleanup_val) {
        break;
      }
      value_release(cleanup_val);
    }
    vm->current_frame = saved_call_stack_size > 0
                            ? &vm->call_stack[saved_call_stack_size - 1]
                            : NULL;
    vm->bytecode = saved_bytecode;
    vm->ip = saved_ip;
    return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
  }

  if (vm->call_stack_size > saved_call_stack_size) {
    callback_frame = &vm->call_stack[vm->call_stack_size - 1];
    cleanup_call_frame_locals(callback_frame);
    if (callback_frame->owned_bytecode) {
      free(callback_frame->owned_bytecode);
      callback_frame->owned_bytecode = NULL;
    }
    vm->call_stack_size--;
  }

  vm->current_frame =
      saved_call_stack_size > 0 ? &vm->call_stack[saved_call_stack_size - 1]
                                : NULL;
  vm->bytecode = saved_bytecode;
  vm->ip = saved_ip;
  *out_result = result;
  return 0;
}

int builtin_read_file(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Expected 1 argument");
  KronosValue *path_val;
  POP_OR_RETURN(vm, path_val);
  if (path_val->type != VAL_STRING) {
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Path must be a string");
  }
  FILE *file = portable_fopen(path_val->as.string.data, "rb");
  if (!file) {
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Could not open file");
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to seek to end of file");
  }
  long fsize = ftell(file);
  if (fsize < 0) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to get file size");
  }
  rewind(file);
  if ((unsigned long)fsize > SIZE_MAX - 1) {
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "File too large");
  }
  char *buff = malloc(fsize + 1);
  if (!buff) {
    fclose(file);
    value_release(path_val);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  size_t bytes_read = fread(buff, 1, fsize, file);
  if (bytes_read != (size_t)fsize) {
    free(buff);
    fclose(file);
    value_release(path_val);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to read file");
  }
  buff[bytes_read] = '\0';
  fclose(file);
  KronosValue *res = value_new_string(buff, bytes_read);
  free(buff);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, res, value_release(res);
                              value_release(path_val););
  value_release(res);
  value_release(path_val);
  return 0;
}

// Built-in function implementations (extracted from handle_op_call_func)
int builtin_add(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'add' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number + b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'add' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

int builtin_subtract(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'subtract' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number - b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'subtract' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

int builtin_multiply(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'multiply' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    KronosValue *result = value_new_number(a->as.number * b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'multiply' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

int builtin_divide(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'divide' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *b;

  POP_OR_RETURN(vm, b);
  KronosValue *a;

  POP_OR_RETURN_WITH_CLEANUP(vm, a, value_release(b));
  if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
    if (b->as.number == 0.0) {
      value_release(a);
      value_release(b);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Division by zero");
    }
    KronosValue *result = value_new_number(a->as.number / b->as.number);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(a); value_release(b););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'divide' requires both arguments to be numbers");
    value_release(a);
    value_release(b);
    return err;
  }
  value_release(a);
  value_release(b);
  return 0;
}

int builtin_len(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'len' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type == VAL_LIST) {
    KronosValue *result = value_new_number((double)arg->as.list.count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else if (arg->type == VAL_STRING) {
    KronosValue *result = value_new_number((double)arg->as.string.length);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else if (arg->type == VAL_RANGE) {
    // Calculate range length: number of values in range
    double start = arg->as.range.start;
    double end = arg->as.range.end;
    double step = arg->as.range.step;

    if (step == 0.0) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_RUNTIME, "Range step cannot be zero");
    }

    // Calculate number of steps
    double diff = end - start;
    double count = floor((diff / step)) + 1.0;

    // Ensure count is non-negative
    if (count < 0) {
      count = 0;
    }

    KronosValue *result = value_new_number(count);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                                value_release(arg););
    value_release(result);
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'len' requires a list, string, or range argument");
    value_release(arg);
    return err;
  }
  value_release(arg);
  return 0;
}

int builtin_uppercase(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'uppercase' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'uppercase' requires a string argument");
    value_release(arg);
    return err;
  }

  char *upper = malloc(arg->as.string.length + 1);
  if (!upper) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  for (size_t i = 0; i < arg->as.string.length; i++) {
    upper[i] = (char)toupper((unsigned char)arg->as.string.data[i]);
  }
  upper[arg->as.string.length] = '\0';

  KronosValue *result = value_new_string(upper, arg->as.string.length);
  free(upper);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_lowercase(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'lowercase' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'lowercase' requires a string argument");
    value_release(arg);
    return err;
  }

  char *lower = malloc(arg->as.string.length + 1);
  if (!lower) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  for (size_t i = 0; i < arg->as.string.length; i++) {
    lower[i] = (char)tolower((unsigned char)arg->as.string.data[i]);
  }
  lower[arg->as.string.length] = '\0';

  KronosValue *result = value_new_string(lower, arg->as.string.length);
  free(lower);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_trim(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'trim' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'trim' requires a string argument");
    value_release(arg);
    return err;
  }

  // Find start (skip leading whitespace)
  size_t start = 0;
  while (start < arg->as.string.length &&
         isspace((unsigned char)arg->as.string.data[start])) {
    start++;
  }

  // Find end (skip trailing whitespace)
  size_t end = arg->as.string.length;
  while (end > start && isspace((unsigned char)arg->as.string.data[end - 1])) {
    end--;
  }

  size_t trimmed_len = end - start;
  char *trimmed = malloc(trimmed_len + 1);
  if (!trimmed) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  memcpy(trimmed, arg->as.string.data + start, trimmed_len);
  trimmed[trimmed_len] = '\0';

  KronosValue *result = value_new_string(trimmed, trimmed_len);
  free(trimmed);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_split(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'split' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *delim;

  POP_OR_RETURN(vm, delim);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(delim));
  if (str->type != VAL_STRING || delim->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'split' requires two string arguments");
    value_release(str);
    value_release(delim);
    return err;
  }

  // Create result list
  KronosValue *result = value_new_list(4);
  if (!result) {
    value_release(str);
    value_release(delim);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  // Handle empty delimiter (split into characters)
  if (delim->as.string.length == 0) {
    for (size_t i = 0; i < str->as.string.length; i++) {
      char ch = str->as.string.data[i];
      KronosValue *char_str = value_new_string(&ch, 1);
      if (!char_str) {
        value_release(result);
        value_release(str);
        value_release(delim);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
      }
      // Grow list if needed
      if (result->as.list.count >= result->as.list.capacity) {
        size_t new_cap = result->as.list.capacity * 2;
        KronosValue **new_items =
            realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
        if (!new_items) {
          value_release(char_str);
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
        }
        result->as.list.items = new_items;
        result->as.list.capacity = new_cap;
      }
      value_retain(char_str);
      result->as.list.items[result->as.list.count++] = char_str;
      value_release(char_str);
    }
  } else {
    // Split by delimiter
    const char *str_data = str->as.string.data;
    size_t str_len = str->as.string.length;
    const char *delim_data = delim->as.string.data;
    size_t delim_len = delim->as.string.length;

    size_t start = 0;
    while (start < str_len) {
      // Find next delimiter
      size_t pos = start;
      bool found = false;
      while (pos + delim_len <= str_len) {
        if (memcmp(str_data + pos, delim_data, delim_len) == 0) {
          found = true;
          break;
        }
        pos++;
      }

      if (found) {
        // Extract substring from start to pos
        size_t substr_len = pos - start;
        char *substr = malloc(substr_len + 1);
        if (!substr) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        memcpy(substr, str_data + start, substr_len);
        substr[substr_len] = '\0';

        KronosValue *substr_val = value_new_string(substr, substr_len);
        free(substr);
        if (!substr_val) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
        }
        // Grow list if needed
        if (result->as.list.count >= result->as.list.capacity) {
          size_t new_cap = result->as.list.capacity * 2;
          KronosValue **new_items =
              realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
          if (!new_items) {
            value_release(substr_val);
            value_release(result);
            value_release(str);
            value_release(delim);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
          }
          result->as.list.items = new_items;
          result->as.list.capacity = new_cap;
        }
        value_retain(substr_val);
        result->as.list.items[result->as.list.count++] = substr_val;
        value_release(substr_val);
        start = pos + delim_len;
      } else {
        // No more delimiters, add remaining string
        size_t substr_len = str_len - start;
        char *substr = malloc(substr_len + 1);
        if (!substr) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
        }
        memcpy(substr, str_data + start, substr_len);
        substr[substr_len] = '\0';

        KronosValue *substr_val = value_new_string(substr, substr_len);
        free(substr);
        if (!substr_val) {
          value_release(result);
          value_release(str);
          value_release(delim);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string");
        }
        // Grow list if needed
        if (result->as.list.count >= result->as.list.capacity) {
          size_t new_cap = result->as.list.capacity * 2;
          KronosValue **new_items =
              realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
          if (!new_items) {
            value_release(substr_val);
            value_release(result);
            value_release(str);
            value_release(delim);
            return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
          }
          result->as.list.items = new_items;
          result->as.list.capacity = new_cap;
        }
        value_retain(substr_val);
        result->as.list.items[result->as.list.count++] = substr_val;
        value_release(substr_val);
        break;
      }
    }
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(delim););
  value_release(result);
  value_release(str);
  value_release(delim);
  return 0;
}

int builtin_join(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'join' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *delim;

  POP_OR_RETURN(vm, delim);
  KronosValue *list;

  POP_OR_RETURN_WITH_CLEANUP(vm, list, value_release(delim));
  if (list->type != VAL_LIST || delim->type != VAL_STRING) {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'join' requires a list and a string delimiter");
    value_release(list);
    value_release(delim);
    return err;
  }

  // Calculate total length
  size_t total_len = 0;
  for (size_t i = 0; i < list->as.list.count; i++) {
    KronosValue *item = list->as.list.items[i];
    if (item->type != VAL_STRING) {
      value_release(list);
      value_release(delim);
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "All list items must be strings for join");
    }
    total_len += item->as.string.length;
    if (i > 0) {
      total_len += delim->as.string.length;
    }
  }

  // Build joined string
  char *joined = malloc(total_len + 1);
  if (!joined) {
    value_release(list);
    value_release(delim);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  size_t offset = 0;
  for (size_t i = 0; i < list->as.list.count; i++) {
    if (i > 0) {
      memcpy(joined + offset, delim->as.string.data, delim->as.string.length);
      offset += delim->as.string.length;
    }
    KronosValue *item = list->as.list.items[i];
    memcpy(joined + offset, item->as.string.data, item->as.string.length);
    offset += item->as.string.length;
  }
  joined[total_len] = '\0';

  KronosValue *result = value_new_string(joined, total_len);
  free(joined);
  if (!result) {
    value_release(list);
    value_release(delim);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(list); value_release(delim););
  value_release(result);
  value_release(list);
  value_release(delim);
  return 0;
}

int builtin_to_string(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_string' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  char *str_buf = NULL;
  size_t str_len = 0;

  if (arg->type == VAL_STRING) {
    // Already a string, just return it
    PUSH_OR_RETURN_WITH_CLEANUP(vm, arg, value_release(arg););
    value_release(arg); // Release the pop reference (push already retained)
    return 0;
  } else if (arg->type == VAL_NUMBER) {
    // Convert number to string
    str_buf = malloc(NUMBER_STRING_BUFFER_SIZE);
    if (!str_buf) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
    }

    // Check if it's a whole number
    double intpart;
    double frac = modf(arg->as.number, &intpart);

    if (frac == 0.0 && fabs(arg->as.number) < 1.0e15) {
      str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%.0f",
                                 arg->as.number);
    } else {
      str_len = (size_t)snprintf(str_buf, NUMBER_STRING_BUFFER_SIZE, "%g",
                                 arg->as.number);
    }
  } else if (arg->type == VAL_BOOL) {
    if (arg->as.boolean) {
      str_buf = strdup("true");
      if (!str_buf) {
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
      }
      str_len = 4;
    } else {
      str_buf = strdup("false");
      if (!str_buf) {
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
      }
      str_len = 5;
    }
  } else if (arg->type == VAL_NIL) {
    str_buf = strdup("null");
    if (!str_buf) {
      value_release(arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
    }
    str_len = 4;
  } else {
    value_release(arg);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Cannot convert type to string");
  }

  KronosValue *result = value_new_string(str_buf, str_len);
  free(str_buf); // Always free our buffer (value_new_string copies it)
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_contains(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'contains' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *substring;

  POP_OR_RETURN(vm, substring);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(substring));
  if (str->type != VAL_STRING || substring->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'contains' requires two string arguments");
    value_release(str);
    value_release(substring);
    return err;
  }

  size_t str_len = str->as.string.length;
  size_t sub_len = substring->as.string.length;
  bool found = false;

  if (sub_len == 0) {
    found = true;
  } else if (sub_len <= str_len) {
    size_t last_start = str_len - sub_len;
    for (size_t i = 0; i <= last_start; i++) {
      if (memcmp(str->as.string.data + i, substring->as.string.data, sub_len) ==
          0) {
        found = true;
        break;
      }
    }
  }

  KronosValue *result = value_new_bool(found);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(substring););
  value_release(result);
  value_release(str);
  value_release(substring);
  return 0;
}

int builtin_starts_with(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'starts_with' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *prefix;

  POP_OR_RETURN(vm, prefix);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(prefix));
  if (str->type != VAL_STRING || prefix->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'starts_with' requires two string arguments");
    value_release(str);
    value_release(prefix);
    return err;
  }

  bool starts = false;
  if (prefix->as.string.length <= str->as.string.length) {
    starts = (memcmp(str->as.string.data, prefix->as.string.data,
                     prefix->as.string.length) == 0);
  }
  KronosValue *result = value_new_bool(starts);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(prefix););
  value_release(result);
  value_release(str);
  value_release(prefix);
  return 0;
}

int builtin_ends_with(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'ends_with' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *suffix;

  POP_OR_RETURN(vm, suffix);
  KronosValue *str;

  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(suffix));
  if (str->type != VAL_STRING || suffix->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'ends_with' requires two string arguments");
    value_release(str);
    value_release(suffix);
    return err;
  }

  bool ends = false;
  if (suffix->as.string.length <= str->as.string.length) {
    size_t start_pos = str->as.string.length - suffix->as.string.length;
    ends = (memcmp(str->as.string.data + start_pos, suffix->as.string.data,
                   suffix->as.string.length) == 0);
  }
  KronosValue *result = value_new_bool(ends);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(suffix););
  value_release(result);
  value_release(str);
  value_release(suffix);
  return 0;
}

int builtin_replace(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 3) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'replace' expects 3 arguments, got %d",
                     arg_count);
  }
  KronosValue *new_str;

  POP_OR_RETURN(vm, new_str);
  KronosValue *old_str;

  POP_OR_RETURN_WITH_CLEANUP(vm, old_str, value_release(new_str));
  KronosValue *str;
  POP_OR_RETURN_WITH_CLEANUP(vm, str, value_release(old_str);
                             value_release(new_str));
  if (str->type != VAL_STRING || old_str->type != VAL_STRING ||
      new_str->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'replace' requires three string arguments");
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return err;
  }

  // Handle empty old string (return original string)
  if (old_str->as.string.length == 0) {
    value_retain(str);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, str, value_release(str);
                                value_release(old_str);
                                value_release(new_str););
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return 0;
  }

  // Calculate maximum possible result size
  size_t str_len = str->as.string.length;
  size_t old_len = old_str->as.string.length;
  size_t new_len = new_str->as.string.length;
  size_t max_result_len = str_len;

  if (new_len > old_len) {
    size_t max_occurrences = str_len / old_len;
    size_t growth_per_occurrence = new_len - old_len;

    if (max_occurrences > 0 &&
        growth_per_occurrence > SIZE_MAX / max_occurrences) {
      max_result_len = SIZE_MAX;
    } else {
      size_t total_growth = max_occurrences * growth_per_occurrence;
      if (total_growth > SIZE_MAX - str_len) {
        max_result_len = SIZE_MAX;
      } else {
        max_result_len = str_len + total_growth;
      }
    }
  }

  // Check for overflow before malloc
  if (max_result_len > SIZE_MAX - 1) {
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Result string too large");
  }

  char *result_buf = malloc(max_result_len + 1);
  if (!result_buf) {
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  size_t result_len = 0;
  const char *search_start = str->as.string.data;
  const char *search_end = str->as.string.data + str->as.string.length;

  while (search_start < search_end) {
    const char *found = NULL;
    size_t remaining = (size_t)(search_end - search_start);

    if (remaining >= old_len) {
      const char *scan = search_start;
      const char *scan_end = search_end - old_len + 1;
      unsigned char first_byte = (unsigned char)old_str->as.string.data[0];

      while (scan < scan_end) {
        const void *candidate_ptr =
            memchr(scan, first_byte, (size_t)(scan_end - scan));
        if (!candidate_ptr) {
          break;
        }

        const char *candidate = (const char *)candidate_ptr;
        if (memcmp(candidate, old_str->as.string.data, old_len) == 0) {
          found = candidate;
          break;
        }

        scan = candidate + 1;
      }
    }

    if (!found) {
      // No more occurrences, copy rest of string
      memcpy(result_buf + result_len, search_start, remaining);
      result_len += remaining;
      break;
    }

    // Copy part before match
    size_t before_len = found - search_start;
    memcpy(result_buf + result_len, search_start, before_len);
    result_len += before_len;

    // Copy replacement
    memcpy(result_buf + result_len, new_str->as.string.data,
           new_str->as.string.length);
    result_len += new_str->as.string.length;

    // Move past the old substring
    search_start = found + old_len;
  }

  result_buf[result_len] = '\0';

  KronosValue *result = value_new_string(result_buf, result_len);
  free(result_buf);
  if (!result) {
    value_release(str);
    value_release(old_str);
    value_release(new_str);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(str); value_release(old_str);
                              value_release(new_str););
  value_release(result);
  value_release(str);
  value_release(old_str);
  value_release(new_str);
  return 0;
}

int builtin_sqrt(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sqrt' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sqrt' requires a number argument");
    value_release(arg);
    return err;
  }
  if (arg->as.number < 0) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sqrt' requires a non-negative number");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(sqrt(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_power(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'power' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *exponent;

  POP_OR_RETURN(vm, exponent);
  KronosValue *base;

  POP_OR_RETURN_WITH_CLEANUP(vm, base, value_release(exponent));
  if (base->type != VAL_NUMBER || exponent->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'power' requires two number arguments");
    value_release(base);
    value_release(exponent);
    return err;
  }
  KronosValue *result =
      value_new_number(pow(base->as.number, exponent->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(base); value_release(exponent););
  value_release(result);
  value_release(base);
  value_release(exponent);
  return 0;
}

int builtin_abs(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'abs' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'abs' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(fabs(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_round(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'round' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'round' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(round(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_floor(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'floor' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'floor' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(floor(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_ceil(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'ceil' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'ceil' requires a number argument");
    value_release(arg);
    return err;
  }
  KronosValue *result = value_new_number(ceil(arg->as.number));
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_rand(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 0) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'rand' expects 0 arguments, got %d", arg_count);
  }
  // Generate random number between 0.0 and 1.0
  double random_val = (double)rand() / (double)RAND_MAX;
  KronosValue *result = value_new_number(random_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_min(KronosVM *vm, uint8_t arg_count) {
  if (arg_count < 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'min' expects at least 1 argument, got %d",
                     arg_count);
  }

  // Pop all arguments
  KronosValue **args = malloc(sizeof(KronosValue *) * arg_count);
  if (!args) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  for (int i = arg_count - 1; i >= 0; i--) {
    args[i] = pop(vm);
    if (!args[i]) {
      for (int j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Validate all are numbers
  for (size_t i = 0; i < arg_count; i++) {
    if (args[i]->type != VAL_NUMBER) {
      int err =
          vm_errorf(vm, KRONOS_ERR_RUNTIME,
                    "Function 'min' requires all arguments to be numbers");
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return err;
    }
  }

  // Find minimum
  double min_val = args[0]->as.number;
  for (size_t i = 1; i < arg_count; i++) {
    if (args[i]->as.number < min_val) {
      min_val = args[i]->as.number;
    }
  }

  // Release all arguments
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  KronosValue *result = value_new_number(min_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_max(KronosVM *vm, uint8_t arg_count) {
  if (arg_count < 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'max' expects at least 1 argument, got %d",
                     arg_count);
  }

  // Pop all arguments
  KronosValue **args = malloc(sizeof(KronosValue *) * arg_count);
  if (!args) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  for (int i = arg_count - 1; i >= 0; i--) {
    args[i] = pop(vm);
    if (!args[i]) {
      for (int j = i + 1; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
  }

  // Validate all are numbers
  for (size_t i = 0; i < arg_count; i++) {
    if (args[i]->type != VAL_NUMBER) {
      int err =
          vm_errorf(vm, KRONOS_ERR_RUNTIME,
                    "Function 'max' requires all arguments to be numbers");
      for (size_t j = 0; j < arg_count; j++) {
        value_release(args[j]);
      }
      free(args);
      return err;
    }
  }

  // Find maximum
  double max_val = args[0]->as.number;
  for (size_t i = 1; i < arg_count; i++) {
    if (args[i]->as.number > max_val) {
      max_val = args[i]->as.number;
    }
  }

  // Release all arguments
  for (size_t i = 0; i < arg_count; i++) {
    value_release(args[i]);
  }
  free(args);

  KronosValue *result = value_new_number(max_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

typedef double (*UnaryMathFunction)(double);

static int builtin_unary_math(KronosVM *vm, uint8_t arg_count,
                              const char *name, UnaryMathFunction function,
                              bool require_unit_interval,
                              bool require_positive) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' expects 1 argument, got %d", name,
                     arg_count);
  }

  KronosValue *arg;
  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_NUMBER) {
    int error = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function '%s' requires a number argument", name);
    value_release(arg);
    return error;
  }
  if (require_unit_interval &&
      (arg->as.number < -1.0 || arg->as.number > 1.0)) {
    int error = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function '%s' requires a value between -1 and 1",
                          name);
    value_release(arg);
    return error;
  }
  if (require_positive && arg->as.number <= 0.0) {
    int error = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function '%s' requires a positive number", name);
    value_release(arg);
    return error;
  }

  KronosValue *result = value_new_number(function(arg->as.number));
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

#define DEFINE_UNARY_MATH_BUILTIN(name, function, unit_interval, positive)     \
  int builtin_##name(KronosVM *vm, uint8_t arg_count) {                       \
    return builtin_unary_math(vm, arg_count, #name, function, unit_interval,  \
                              positive);                                      \
  }

DEFINE_UNARY_MATH_BUILTIN(sin, sin, false, false)
DEFINE_UNARY_MATH_BUILTIN(cos, cos, false, false)
DEFINE_UNARY_MATH_BUILTIN(tan, tan, false, false)
DEFINE_UNARY_MATH_BUILTIN(asin, asin, true, false)
DEFINE_UNARY_MATH_BUILTIN(acos, acos, true, false)
DEFINE_UNARY_MATH_BUILTIN(atan, atan, false, false)
DEFINE_UNARY_MATH_BUILTIN(log, log, false, true)
DEFINE_UNARY_MATH_BUILTIN(log10, log10, false, true)
DEFINE_UNARY_MATH_BUILTIN(exp, exp, false, false)
DEFINE_UNARY_MATH_BUILTIN(cbrt, cbrt, false, false)

#undef DEFINE_UNARY_MATH_BUILTIN

static int pop_string_pair(KronosVM *vm, uint8_t arg_count, const char *name,
                           KronosValue **text, KronosValue **needle) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' expects 2 arguments, got %d", name,
                     arg_count);
  }
  POP_OR_RETURN(vm, *needle);
  POP_OR_RETURN_WITH_CLEANUP(vm, *text, value_release(*needle));
  if ((*text)->type != VAL_STRING || (*needle)->type != VAL_STRING) {
    int error = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function '%s' requires string arguments", name);
    value_release(*text);
    value_release(*needle);
    return error;
  }
  return 0;
}

int builtin_find(KronosVM *vm, uint8_t arg_count) {
  KronosValue *text;
  KronosValue *needle;
  int status = pop_string_pair(vm, arg_count, "find", &text, &needle);
  if (status != 0)
    return status;
  const char *match = strstr(text->as.string.data, needle->as.string.data);
  double index = match ? (double)(match - text->as.string.data) : -1.0;
  KronosValue *result = value_new_number(index);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(text); value_release(needle););
  value_release(result);
  value_release(text);
  value_release(needle);
  return 0;
}

int builtin_rfind(KronosVM *vm, uint8_t arg_count) {
  KronosValue *text;
  KronosValue *needle;
  int status = pop_string_pair(vm, arg_count, "rfind", &text, &needle);
  if (status != 0)
    return status;
  const char *last = NULL;
  const char *cursor = text->as.string.data;
  while (true) {
    const char *match = strstr(cursor, needle->as.string.data);
    if (!match)
      break;
    last = match;
    cursor = match + (needle->as.string.length > 0 ? 1 : 0);
    if (needle->as.string.length == 0 && *cursor == '\0')
      break;
  }
  double index = last ? (double)(last - text->as.string.data) : -1.0;
  KronosValue *result = value_new_number(index);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(text); value_release(needle););
  value_release(result);
  value_release(text);
  value_release(needle);
  return 0;
}

int builtin_count(KronosVM *vm, uint8_t arg_count) {
  KronosValue *text;
  KronosValue *needle;
  int status = pop_string_pair(vm, arg_count, "count", &text, &needle);
  if (status != 0)
    return status;
  size_t count = 0;
  if (needle->as.string.length > 0) {
    const char *cursor = text->as.string.data;
    const char *match;
    while ((match = strstr(cursor, needle->as.string.data)) != NULL) {
      count++;
      cursor = match + needle->as.string.length;
    }
  } else {
    count = text->as.string.length + 1;
  }
  KronosValue *result = value_new_number((double)count);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(text); value_release(needle););
  value_release(result);
  value_release(text);
  value_release(needle);
  return 0;
}

static int builtin_string_case(KronosVM *vm, uint8_t arg_count,
                               const char *name, bool title_case) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' expects 1 argument, got %d", name,
                     arg_count);
  KronosValue *arg;
  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_STRING) {
    int error = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function '%s' requires a string argument", name);
    value_release(arg);
    return error;
  }
  char *buffer = malloc(arg->as.string.length + 1);
  if (!buffer) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }
  bool new_word = true;
  for (size_t i = 0; i < arg->as.string.length; i++) {
    unsigned char character = (unsigned char)arg->as.string.data[i];
    buffer[i] = (char)(new_word ? toupper(character) : tolower(character));
    new_word = title_case && isspace(character);
  }
  buffer[arg->as.string.length] = '\0';
  KronosValue *result = value_new_string(buffer, arg->as.string.length);
  free(buffer);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_capitalize(KronosVM *vm, uint8_t arg_count) {
  return builtin_string_case(vm, arg_count, "capitalize", false);
}

int builtin_title(KronosVM *vm, uint8_t arg_count) {
  return builtin_string_case(vm, arg_count, "title", true);
}

static int push_list_result(KronosVM *vm, KronosValue *result) {
  if (!result)
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  int status = push(vm, result);
  value_release(result);
  return status;
}

int builtin_enumerate(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'enumerate' expects 1 argument, got %d",
                     arg_count);
  KronosValue *list;
  POP_OR_RETURN(vm, list);
  if (list->type != VAL_LIST) {
    value_release(list);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'enumerate' requires a list argument");
  }
  KronosValue *result = value_new_list(list->as.list.count);
  if (!result) {
    value_release(list);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  }
  for (size_t i = 0; i < list->as.list.count; i++) {
    KronosValue *index = value_new_number((double)i);
    KronosValue *items[2] = {index, list->as.list.items[i]};
    KronosValue *tuple = index ? value_new_tuple(items, 2) : NULL;
    value_release(index);
    if (!tuple) {
      value_release(result);
      value_release(list);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
    }
    result->as.list.items[result->as.list.count++] = tuple;
  }
  value_release(list);
  return push_list_result(vm, result);
}

int builtin_zip(KronosVM *vm, uint8_t arg_count) {
  if (arg_count < 2)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'zip' expects at least 2 arguments, got %d",
                     arg_count);
  KronosValue **lists = malloc(sizeof(KronosValue *) * arg_count);
  if (!lists)
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  size_t shortest = SIZE_MAX;
  for (int i = arg_count - 1; i >= 0; i--) {
    lists[i] = pop(vm);
    if (!lists[i]) {
      for (int j = i + 1; j < arg_count; j++)
        value_release(lists[j]);
      free(lists);
      return vm_propagate_error(vm, KRONOS_ERR_RUNTIME);
    }
    if (lists[i]->type != VAL_LIST) {
      for (int j = i; j < arg_count; j++)
        value_release(lists[j]);
      free(lists);
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "Function 'zip' requires list arguments");
    }
    if (lists[i]->as.list.count < shortest)
      shortest = lists[i]->as.list.count;
  }
  KronosValue *result = value_new_list(shortest);
  for (size_t row = 0; result && row < shortest; row++) {
    KronosValue **items = malloc(sizeof(KronosValue *) * arg_count);
    if (!items) {
      value_release(result);
      result = NULL;
      break;
    }
    for (size_t column = 0; column < arg_count; column++)
      items[column] = lists[column]->as.list.items[row];
    KronosValue *tuple = value_new_tuple(items, arg_count);
    free(items);
    if (!tuple) {
      value_release(result);
      result = NULL;
      break;
    }
    result->as.list.items[result->as.list.count++] = tuple;
  }
  for (size_t i = 0; i < arg_count; i++)
    value_release(lists[i]);
  free(lists);
  return push_list_result(vm, result);
}

static int builtin_list_predicate(KronosVM *vm, uint8_t arg_count,
                                  const char *name, bool require_all) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' expects 1 argument, got %d", name,
                     arg_count);
  KronosValue *list;
  POP_OR_RETURN(vm, list);
  if (list->type != VAL_LIST) {
    value_release(list);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function '%s' requires a list argument", name);
  }
  bool value = require_all;
  for (size_t i = 0; i < list->as.list.count; i++) {
    bool truthy = value_is_truthy(list->as.list.items[i]);
    if (truthy != require_all) {
      value = !require_all;
      break;
    }
  }
  KronosValue *result = value_new_bool(value);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(list););
  value_release(result);
  value_release(list);
  return 0;
}

int builtin_any(KronosVM *vm, uint8_t arg_count) {
  return builtin_list_predicate(vm, arg_count, "any", false);
}

int builtin_all(KronosVM *vm, uint8_t arg_count) {
  return builtin_list_predicate(vm, arg_count, "all", true);
}

int builtin_sum(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sum' expects 1 argument, got %d", arg_count);
  KronosValue *list;
  POP_OR_RETURN(vm, list);
  if (list->type != VAL_LIST) {
    value_release(list);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'sum' requires a list argument");
  }
  double sum = 0.0;
  for (size_t i = 0; i < list->as.list.count; i++) {
    if (list->as.list.items[i]->type != VAL_NUMBER) {
      value_release(list);
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "Function 'sum' requires a list of numbers");
    }
    sum += list->as.list.items[i]->as.number;
  }
  KronosValue *result = value_new_number(sum);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(list););
  value_release(result);
  value_release(list);
  return 0;
}

int builtin_now(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 0)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'now' expects 0 arguments, got %d", arg_count);
  KronosValue *result = value_new_number((double)time(NULL));
  if (!result)
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_format_date(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'format_date' expects 2 arguments, got %d",
                     arg_count);
  KronosValue *format;
  KronosValue *timestamp;
  POP_OR_RETURN(vm, format);
  POP_OR_RETURN_WITH_CLEANUP(vm, timestamp, value_release(format));
  if (timestamp->type != VAL_NUMBER || format->type != VAL_STRING) {
    value_release(timestamp);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'format_date' requires a number and a string");
  }
  time_t raw_time = (time_t)timestamp->as.number;
  struct tm local_time;
#ifdef _WIN32
  if (localtime_s(&local_time, &raw_time) != 0) {
#else
  if (!localtime_r(&raw_time, &local_time)) {
#endif
    value_release(timestamp);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Invalid date value");
  }
  char buffer[1024];
  size_t length = strftime(buffer, sizeof(buffer), format->as.string.data,
                           &local_time);
  if (length == 0 && format->as.string.length > 0) {
    value_release(timestamp);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Formatted date exceeds maximum length");
  }
  KronosValue *result = value_new_string(buffer, length);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(timestamp); value_release(format););
  value_release(result);
  value_release(timestamp);
  value_release(format);
  return 0;
}

int builtin_parse_date(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'parse_date' expects 2 arguments, got %d",
                     arg_count);
  KronosValue *format;
  KronosValue *text;
  POP_OR_RETURN(vm, format);
  POP_OR_RETURN_WITH_CLEANUP(vm, text, value_release(format));
  if (text->type != VAL_STRING || format->type != VAL_STRING) {
    value_release(text);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'parse_date' requires string arguments");
  }
#ifdef _WIN32
  value_release(text);
  value_release(format);
  return vm_error(vm, KRONOS_ERR_RUNTIME,
                  "Function 'parse_date' is unavailable on this platform");
#else
  struct tm parsed = {0};
  char *end = strptime(text->as.string.data, format->as.string.data, &parsed);
  if (!end || *end != '\0') {
    value_release(text);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Date does not match the supplied format");
  }
  parsed.tm_isdst = -1;
  time_t timestamp_value = mktime(&parsed);
  if (timestamp_value == (time_t)-1) {
    value_release(text);
    value_release(format);
    return vm_error(vm, KRONOS_ERR_RUNTIME, "Date is outside the valid range");
  }
  KronosValue *result = value_new_number((double)timestamp_value);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(text); value_release(format););
  value_release(result);
  value_release(text);
  value_release(format);
  return 0;
#endif
}

int builtin_sleep(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sleep' expects 1 argument, got %d", arg_count);
  KronosValue *duration;
  POP_OR_RETURN(vm, duration);
  if (duration->type != VAL_NUMBER || duration->as.number < 0.0 ||
      !isfinite(duration->as.number)) {
    value_release(duration);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'sleep' requires a non-negative finite number");
  }
  double integral;
  double fractional = modf(duration->as.number, &integral);
  struct timespec requested = {(time_t)integral,
                               (long)(fractional * 1000000000.0)};
  while (nanosleep(&requested, &requested) != 0 && errno == EINTR) {
  }
  value_release(duration);
  KronosValue *result = value_new_nil();
  if (!result)
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_args(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 0)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'args' expects 0 arguments, got %d", arg_count);
  KronosVM *owner = vm->root_vm_ref ? vm->root_vm_ref : vm;
  KronosValue *result = value_new_list(owner->process_arg_count);
  if (!result)
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  for (size_t i = 0; i < owner->process_arg_count; i++) {
    KronosValue *arg = value_new_string(owner->process_args[i],
                                        strlen(owner->process_args[i]));
    if (!arg) {
      value_release(result);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
    }
    result->as.list.items[result->as.list.count++] = arg;
  }
  return push_list_result(vm, result);
}

int builtin_env(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'env' expects 1 argument, got %d", arg_count);
  KronosValue *name;
  POP_OR_RETURN(vm, name);
  if (name->type != VAL_STRING) {
    value_release(name);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'env' requires a string argument");
  }
  const char *value = getenv(name->as.string.data);
  KronosValue *result = value ? value_new_string(value, strlen(value))
                              : value_new_nil();
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(name););
  value_release(result);
  value_release(name);
  return 0;
}

int builtin_exit(KronosVM *vm, uint8_t arg_count) {
  if (arg_count > 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'exit' expects at most 1 argument, got %d",
                     arg_count);
  int exit_code = 0;
  if (arg_count == 1) {
    KronosValue *code;
    POP_OR_RETURN(vm, code);
    if (code->type != VAL_NUMBER || !isfinite(code->as.number) ||
        floor(code->as.number) != code->as.number || code->as.number < 0.0 ||
        code->as.number > 255.0) {
      value_release(code);
      return vm_error(vm, KRONOS_ERR_RUNTIME,
                      "Function 'exit' requires an integer from 0 to 255");
    }
    exit_code = (int)code->as.number;
    value_release(code);
  }
  KronosVM *owner = vm->root_vm_ref ? vm->root_vm_ref : vm;
  owner->exit_requested = true;
  owner->exit_code = exit_code;
  vm->exit_requested = true;
  vm->exit_code = exit_code;
  return 0;
}

#define JSON_MAX_DEPTH 64

typedef struct {
  const char *cursor;
  const char *error;
} JsonParser;

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} JsonBuffer;

static bool json_buffer_append(JsonBuffer *buffer, const char *text,
                               size_t length) {
  if (length > SIZE_MAX - buffer->length - 1)
    return false;
  size_t required = buffer->length + length + 1;
  if (required > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < required) {
      if (capacity > SIZE_MAX / 2)
        return false;
      capacity *= 2;
    }
    char *data = realloc(buffer->data, capacity);
    if (!data)
      return false;
    buffer->data = data;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, text, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return true;
}

static void json_skip_space(JsonParser *parser) {
  while (isspace((unsigned char)*parser->cursor))
    parser->cursor++;
}

static bool json_append_utf8(JsonBuffer *buffer, unsigned codepoint) {
  char bytes[4];
  size_t length;
  if (codepoint <= 0x7f) {
    bytes[0] = (char)codepoint;
    length = 1;
  } else if (codepoint <= 0x7ff) {
    bytes[0] = (char)(0xc0 | (codepoint >> 6));
    bytes[1] = (char)(0x80 | (codepoint & 0x3f));
    length = 2;
  } else {
    bytes[0] = (char)(0xe0 | (codepoint >> 12));
    bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    bytes[2] = (char)(0x80 | (codepoint & 0x3f));
    length = 3;
  }
  return json_buffer_append(buffer, bytes, length);
}

static int json_hex_digit(char character) {
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return character - 'a' + 10;
  if (character >= 'A' && character <= 'F')
    return character - 'A' + 10;
  return -1;
}

static KronosValue *json_parse_string(JsonParser *parser) {
  if (*parser->cursor++ != '"')
    return NULL;
  JsonBuffer buffer = {0};
  while (*parser->cursor && *parser->cursor != '"') {
    unsigned char character = (unsigned char)*parser->cursor++;
    if (character < 0x20) {
      parser->error = "Unescaped control character in JSON string";
      free(buffer.data);
      return NULL;
    }
    if (character != '\\') {
      if (!json_buffer_append(&buffer, (const char *)&character, 1))
        parser->error = "Failed to allocate JSON string";
      if (parser->error) {
        free(buffer.data);
        return NULL;
      }
      continue;
    }
    char escape = *parser->cursor++;
    const char *replacement = NULL;
    switch (escape) {
    case '"': replacement = "\""; break;
    case '\\': replacement = "\\"; break;
    case '/': replacement = "/"; break;
    case 'b': replacement = "\b"; break;
    case 'f': replacement = "\f"; break;
    case 'n': replacement = "\n"; break;
    case 'r': replacement = "\r"; break;
    case 't': replacement = "\t"; break;
    case 'u': {
      unsigned codepoint = 0;
      for (int i = 0; i < 4; i++) {
        int digit = json_hex_digit(parser->cursor[i]);
        if (digit < 0) {
          parser->error = "Invalid Unicode escape in JSON string";
          free(buffer.data);
          return NULL;
        }
        codepoint = codepoint * 16u + (unsigned)digit;
      }
      parser->cursor += 4;
      if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
        parser->error = "JSON surrogate pairs are not supported";
        free(buffer.data);
        return NULL;
      }
      if (!json_append_utf8(&buffer, codepoint)) {
        parser->error = "Failed to allocate JSON string";
        free(buffer.data);
        return NULL;
      }
      continue;
    }
    default:
      parser->error = "Invalid escape in JSON string";
      free(buffer.data);
      return NULL;
    }
    if (!json_buffer_append(&buffer, replacement, 1)) {
      parser->error = "Failed to allocate JSON string";
      free(buffer.data);
      return NULL;
    }
  }
  if (*parser->cursor != '"') {
    parser->error = "Unterminated JSON string";
    free(buffer.data);
    return NULL;
  }
  parser->cursor++;
  KronosValue *value = value_new_string(buffer.data ? buffer.data : "",
                                        buffer.length);
  free(buffer.data);
  if (!value)
    parser->error = "Failed to allocate JSON value";
  return value;
}

static KronosValue *json_parse_value(JsonParser *parser, int depth);

static KronosValue *json_parse_number(JsonParser *parser) {
  const char *start = parser->cursor;
  const char *cursor = start;
  if (*cursor == '-')
    cursor++;
  if (*cursor == '0') {
    cursor++;
    if (isdigit((unsigned char)*cursor)) {
      parser->error = "JSON numbers cannot contain leading zeros";
      return NULL;
    }
  } else if (*cursor >= '1' && *cursor <= '9') {
    while (isdigit((unsigned char)*cursor))
      cursor++;
  } else {
    return NULL;
  }
  if (*cursor == '.') {
    cursor++;
    if (!isdigit((unsigned char)*cursor)) {
      parser->error = "JSON fraction requires a digit";
      return NULL;
    }
    while (isdigit((unsigned char)*cursor))
      cursor++;
  }
  if (*cursor == 'e' || *cursor == 'E') {
    cursor++;
    if (*cursor == '+' || *cursor == '-')
      cursor++;
    if (!isdigit((unsigned char)*cursor)) {
      parser->error = "JSON exponent requires a digit";
      return NULL;
    }
    while (isdigit((unsigned char)*cursor))
      cursor++;
  }

  errno = 0;
  char *end;
  double number = strtod(start, &end);
  if (end != cursor || errno == ERANGE || !isfinite(number)) {
    parser->error = "JSON number is outside the supported range";
    return NULL;
  }
  parser->cursor = cursor;
  return value_new_number(number);
}

static KronosValue *json_parse_array(JsonParser *parser, int depth) {
  parser->cursor++;
  KronosValue *list = value_new_list(4);
  if (!list) {
    parser->error = "Failed to allocate JSON array";
    return NULL;
  }
  json_skip_space(parser);
  if (*parser->cursor == ']') {
    parser->cursor++;
    return list;
  }
  while (true) {
    KronosValue *item = json_parse_value(parser, depth + 1);
    if (!item) {
      value_release(list);
      return NULL;
    }
    if (list->as.list.count == list->as.list.capacity) {
      size_t capacity = list->as.list.capacity * 2;
      KronosValue **items = realloc(list->as.list.items,
                                    capacity * sizeof(KronosValue *));
      if (!items) {
        value_release(item);
        value_release(list);
        parser->error = "Failed to grow JSON array";
        return NULL;
      }
      list->as.list.items = items;
      list->as.list.capacity = capacity;
    }
    list->as.list.items[list->as.list.count++] = item;
    json_skip_space(parser);
    if (*parser->cursor == ']') {
      parser->cursor++;
      return list;
    }
    if (*parser->cursor++ != ',') {
      value_release(list);
      parser->error = "Expected ',' or ']' in JSON array";
      return NULL;
    }
    json_skip_space(parser);
  }
}

static KronosValue *json_parse_object(JsonParser *parser, int depth) {
  parser->cursor++;
  KronosValue *map = value_new_map(8);
  if (!map) {
    parser->error = "Failed to allocate JSON object";
    return NULL;
  }
  json_skip_space(parser);
  if (*parser->cursor == '}') {
    parser->cursor++;
    return map;
  }
  while (true) {
    if (*parser->cursor != '"') {
      value_release(map);
      parser->error = "JSON object keys must be strings";
      return NULL;
    }
    KronosValue *key = json_parse_string(parser);
    json_skip_space(parser);
    if (!key || *parser->cursor++ != ':') {
      value_release(key);
      value_release(map);
      if (!parser->error)
        parser->error = "Expected ':' after JSON object key";
      return NULL;
    }
    KronosValue *value = json_parse_value(parser, depth + 1);
    if (!value || map_set(map, key, value) != 0) {
      value_release(key);
      value_release(value);
      value_release(map);
      if (!parser->error)
        parser->error = "Failed to add JSON object entry";
      return NULL;
    }
    value_release(key);
    value_release(value);
    json_skip_space(parser);
    if (*parser->cursor == '}') {
      parser->cursor++;
      return map;
    }
    if (*parser->cursor++ != ',') {
      value_release(map);
      parser->error = "Expected ',' or '}' in JSON object";
      return NULL;
    }
    json_skip_space(parser);
  }
}

static KronosValue *json_parse_value(JsonParser *parser, int depth) {
  if (depth > JSON_MAX_DEPTH) {
    parser->error = "JSON nesting exceeds maximum depth";
    return NULL;
  }
  json_skip_space(parser);
  if (*parser->cursor == '"')
    return json_parse_string(parser);
  if (*parser->cursor == '[')
    return json_parse_array(parser, depth);
  if (*parser->cursor == '{')
    return json_parse_object(parser, depth);
  if (strncmp(parser->cursor, "true", 4) == 0) {
    parser->cursor += 4;
    return value_new_bool(true);
  }
  if (strncmp(parser->cursor, "false", 5) == 0) {
    parser->cursor += 5;
    return value_new_bool(false);
  }
  if (strncmp(parser->cursor, "null", 4) == 0) {
    parser->cursor += 4;
    return value_new_nil();
  }
  KronosValue *number = json_parse_number(parser);
  if (number)
    return number;
  if (parser->error)
    return NULL;
  parser->error = "Invalid JSON value";
  return NULL;
}

static bool json_serialize_value(JsonBuffer *buffer, KronosValue *value,
                                 int depth);

static bool json_serialize_string(JsonBuffer *buffer, const char *text,
                                  size_t length) {
  if (!json_buffer_append(buffer, "\"", 1))
    return false;
  for (size_t i = 0; i < length; i++) {
    unsigned char character = (unsigned char)text[i];
    const char *escape = NULL;
    switch (character) {
    case '"': escape = "\\\""; break;
    case '\\': escape = "\\\\"; break;
    case '\b': escape = "\\b"; break;
    case '\f': escape = "\\f"; break;
    case '\n': escape = "\\n"; break;
    case '\r': escape = "\\r"; break;
    case '\t': escape = "\\t"; break;
    }
    if (escape) {
      if (!json_buffer_append(buffer, escape, 2))
        return false;
    } else if (character < 0x20) {
      char unicode[7];
      snprintf(unicode, sizeof(unicode), "\\u%04x", character);
      if (!json_buffer_append(buffer, unicode, 6))
        return false;
    } else if (!json_buffer_append(buffer, (const char *)&character, 1)) {
      return false;
    }
  }
  return json_buffer_append(buffer, "\"", 1);
}

static bool json_serialize_value(JsonBuffer *buffer, KronosValue *value,
                                 int depth) {
  if (!value || depth > JSON_MAX_DEPTH)
    return false;
  char number[64];
  switch (value->type) {
  case VAL_NIL:
    return json_buffer_append(buffer, "null", 4);
  case VAL_BOOL:
    return json_buffer_append(buffer, value->as.boolean ? "true" : "false",
                              value->as.boolean ? 4 : 5);
  case VAL_NUMBER: {
    if (!isfinite(value->as.number))
      return false;
    int length = snprintf(number, sizeof(number), "%.17g", value->as.number);
    return length > 0 && json_buffer_append(buffer, number, (size_t)length);
  }
  case VAL_STRING:
    return json_serialize_string(buffer, value->as.string.data,
                                 value->as.string.length);
  case VAL_LIST:
  case VAL_TUPLE: {
    size_t count = value->type == VAL_LIST ? value->as.list.count
                                           : value->as.tuple.count;
    KronosValue **items = value->type == VAL_LIST ? value->as.list.items
                                                   : value->as.tuple.items;
    if (!json_buffer_append(buffer, "[", 1))
      return false;
    for (size_t i = 0; i < count; i++) {
      if ((i > 0 && !json_buffer_append(buffer, ",", 1)) ||
          !json_serialize_value(buffer, items[i], depth + 1))
        return false;
    }
    return json_buffer_append(buffer, "]", 1);
  }
  case VAL_MAP:
    if (!json_buffer_append(buffer, "{", 1))
      return false;
    size_t written = 0;
    for (size_t i = 0; i < value->as.map.capacity; i++) {
      if (!value->as.map.entries[i].key || value->as.map.entries[i].is_tombstone)
        continue;
      KronosValue *key = value->as.map.entries[i].key;
      if (key->type != VAL_STRING ||
          (written++ > 0 && !json_buffer_append(buffer, ",", 1)) ||
          !json_serialize_string(buffer, key->as.string.data,
                                 key->as.string.length) ||
          !json_buffer_append(buffer, ":", 1) ||
          !json_serialize_value(buffer, value->as.map.entries[i].value,
                                depth + 1))
        return false;
    }
    return json_buffer_append(buffer, "}", 1);
  default:
    return false;
  }
}

int builtin_parse_json(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'parse_json' expects 1 argument, got %d",
                     arg_count);
  KronosValue *text;
  POP_OR_RETURN(vm, text);
  if (text->type != VAL_STRING) {
    value_release(text);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Function 'parse_json' requires a string argument");
  }
  JsonParser parser = {text->as.string.data, NULL};
  KronosValue *result = json_parse_value(&parser, 0);
  json_skip_space(&parser);
  if (!result || *parser.cursor != '\0') {
    value_release(result);
    value_release(text);
    return vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid JSON: %s",
                     parser.error ? parser.error : "unexpected trailing data");
  }
  value_release(text);
  int status = push(vm, result);
  value_release(result);
  return status;
}

int builtin_to_json(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1)
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_json' expects 1 argument, got %d",
                     arg_count);
  KronosValue *value;
  POP_OR_RETURN(vm, value);
  JsonBuffer buffer = {0};
  if (!json_serialize_value(&buffer, value, 0)) {
    free(buffer.data);
    value_release(value);
    return vm_error(vm, KRONOS_ERR_RUNTIME,
                    "Value cannot be represented as JSON");
  }
  KronosValue *result = value_new_string(buffer.data, buffer.length);
  free(buffer.data);
  if (!result) {
    value_release(value);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate result");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(value););
  value_release(result);
  value_release(value);
  return 0;
}

int builtin_to_number(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_number' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  if (arg->type == VAL_NUMBER) {
    // Already a number, just return it
    PUSH_OR_RETURN_WITH_CLEANUP(vm, arg, value_release(arg););
    value_release(arg); // Release the pop reference (push already retained)
    return 0;
  } else if (arg->type == VAL_STRING) {
    // Try to parse string as number
    char *endptr;
    const char *input = arg->as.string.data;
    double num = strtod(input, &endptr);

    if (endptr == input) {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Cannot convert string to number: '%s'",
                          arg->as.string.data);
      value_release(arg);
      return err;
    }

    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' ||
           *endptr == '\n') {
      endptr++;
    }

    // Check if conversion was successful (endptr should point to end of string)
    if (*endptr != '\0') {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Cannot convert string to number: '%s'",
                          arg->as.string.data);
      value_release(arg);
      return err;
    }
    value_release(arg);
    KronosValue *result = value_new_number(num);
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  } else {
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME,
                  "Function 'to_number' requires a string or number argument");
    value_release(arg);
    return err;
  }
}

int builtin_to_bool(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'to_bool' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);

  bool bool_val = false;
  if (arg->type == VAL_BOOL) {
    bool_val = arg->as.boolean;
  } else if (arg->type == VAL_NUMBER) {
    bool_val = (arg->as.number != 0.0);
  } else if (arg->type == VAL_STRING) {
    bool_val = (arg->as.string.length > 0);
  } else if (arg->type == VAL_LIST) {
    bool_val = (arg->as.list.count > 0);
  } else if (arg->type == VAL_NIL) {
    bool_val = false;
  } else {
    bool_val = true; // Other types are truthy
  }

  value_release(arg);
  KronosValue *result = value_new_bool(bool_val);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_reverse(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'reverse' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'reverse' requires a list argument");
    value_release(arg);
    return err;
  }
  // Create new list with reversed items
  KronosValue *result = value_new_list(arg->as.list.count);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }
  // Copy items in reverse order
  for (int i = (int)arg->as.list.count - 1; i >= 0; i--) {
    value_retain(arg->as.list.items[i]);
    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(arg->as.list.items[i]);
        value_release(result);
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }
    result->as.list.items[result->as.list.count++] = arg->as.list.items[i];
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_sort(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'sort' expects 1 argument, got %d", arg_count);
  }
  KronosValue *arg;

  POP_OR_RETURN(vm, arg);
  if (arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'sort' requires a list argument");
    value_release(arg);
    return err;
  }
  // Create new list with sorted items
  KronosValue *result = value_new_list(arg->as.list.count);
  if (!result) {
    value_release(arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }
  // Copy items
  for (size_t i = 0; i < arg->as.list.count; i++) {
    value_retain(arg->as.list.items[i]);
    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(arg->as.list.items[i]);
        value_release(result);
        value_release(arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }
    result->as.list.items[result->as.list.count++] = arg->as.list.items[i];
  }
  // Sort the new list in-place using qsort (O(n log n) average)
  // First, validate all elements are the same type
  if (result->as.list.count > 0) {
    ValueType first_type = result->as.list.items[0]->type;
    if (first_type != VAL_NUMBER && first_type != VAL_STRING) {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Function 'sort' requires list items to be "
                          "all numbers or all strings");
      value_release(result);
      value_release(arg);
      return err;
    }

    // Check all elements are the same type
    for (size_t i = 1; i < result->as.list.count; i++) {
      if (result->as.list.items[i]->type != first_type) {
        int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                            "Function 'sort' requires list items to be "
                            "all numbers or all strings");
        value_release(result);
        value_release(arg);
        return err;
      }
    }

    // Sort using thread-safe comparison (no global state needed)
    // All items are validated to be the same type, so comparison
    // function can determine type from the values themselves
    qsort(result->as.list.items, result->as.list.count, sizeof(KronosValue *),
          sort_compare_values);
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(arg););
  value_release(result);
  value_release(arg);
  return 0;
}

int builtin_filter(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'filter' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *callback_arg;
  POP_OR_RETURN(vm, callback_arg);
  KronosValue *list_arg;
  POP_OR_RETURN_WITH_CLEANUP(vm, list_arg, value_release(callback_arg));

  if (list_arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'filter' requires a list argument");
    value_release(callback_arg);
    value_release(list_arg);
    return err;
  }
  if (callback_arg->type != VAL_FUNCTION) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'filter' requires a function argument");
    value_release(callback_arg);
    value_release(list_arg);
    return err;
  }

  KronosValue *result = value_new_list(list_arg->as.list.count);
  if (!result) {
    value_release(callback_arg);
    value_release(list_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  for (size_t i = 0; i < list_arg->as.list.count; i++) {
    KronosValue *callback_args[1] = {list_arg->as.list.items[i]};
    KronosValue *callback_result = NULL;
    int status = run_function_callback(vm, "filter callback", callback_arg,
                                       callback_args, 1, &callback_result);
    if (status != 0) {
      value_release(result);
      value_release(callback_arg);
      value_release(list_arg);
      return status;
    }

    bool keep_item = value_is_truthy(callback_result);
    value_release(callback_result);
    if (!keep_item) {
      continue;
    }

    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(result);
        value_release(callback_arg);
        value_release(list_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    value_retain(list_arg->as.list.items[i]);
    result->as.list.items[result->as.list.count++] = list_arg->as.list.items[i];
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(callback_arg);
                              value_release(list_arg););
  value_release(result);
  value_release(callback_arg);
  value_release(list_arg);
  return 0;
}

int builtin_map(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'map' expects 2 arguments, got %d", arg_count);
  }
  KronosValue *callback_arg;
  POP_OR_RETURN(vm, callback_arg);
  KronosValue *list_arg;
  POP_OR_RETURN_WITH_CLEANUP(vm, list_arg, value_release(callback_arg));

  if (list_arg->type != VAL_LIST) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'map' requires a list argument");
    value_release(callback_arg);
    value_release(list_arg);
    return err;
  }
  if (callback_arg->type != VAL_FUNCTION) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'map' requires a function argument");
    value_release(callback_arg);
    value_release(list_arg);
    return err;
  }

  KronosValue *result = value_new_list(list_arg->as.list.count);
  if (!result) {
    value_release(callback_arg);
    value_release(list_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  for (size_t i = 0; i < list_arg->as.list.count; i++) {
    KronosValue *callback_args[1] = {list_arg->as.list.items[i]};
    KronosValue *mapped_value = NULL;
    int status = run_function_callback(vm, "map callback", callback_arg,
                                       callback_args, 1, &mapped_value);
    if (status != 0) {
      value_release(result);
      value_release(callback_arg);
      value_release(list_arg);
      return status;
    }

    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(mapped_value);
        value_release(result);
        value_release(callback_arg);
        value_release(list_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    // mapped_value comes from stack pop(), transfer ownership to result list.
    result->as.list.items[result->as.list.count++] = mapped_value;
  }

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(callback_arg);
                              value_release(list_arg););
  value_release(result);
  value_release(callback_arg);
  value_release(list_arg);
  return 0;
}

int builtin_write_file(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'write_file' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *content_arg;

  POP_OR_RETURN(vm, content_arg);
  KronosValue *path_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, path_arg, value_release(content_arg));
  if (path_arg->type != VAL_STRING || content_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'write_file' requires two string arguments");
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  FILE *file = portable_fopen(path_arg->as.string.data, "wb");
  if (!file) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Failed to open file '%s' for writing",
                        path_arg->as.string.data);
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  size_t bytes_written = fwrite(content_arg->as.string.data, 1,
                                content_arg->as.string.length, file);
  int close_status = fclose(file);

  if (bytes_written != content_arg->as.string.length || close_status != 0) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Failed to write all content to file '%s'",
                        path_arg->as.string.data);
    value_release(path_arg);
    value_release(content_arg);
    return err;
  }

  // Return nil (success)
  KronosValue *result = value_new_nil();
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(path_arg);
                              value_release(content_arg););
  value_release(result);
  value_release(path_arg);
  value_release(content_arg);
  return 0;
}

int builtin_read_lines(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'read_lines' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'read_lines' requires a string argument");
    value_release(path_arg);
    return err;
  }

  FILE *file = portable_fopen(path_arg->as.string.data, "r");
  if (!file) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open file '%s'",
                        path_arg->as.string.data);
    value_release(path_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    fclose(file);
    value_release(path_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  char *line = NULL;
  size_t line_len = 0;
  ssize_t read;

  // Use portable getline() implementation for cross-platform compatibility
  while ((read = KRONOS_GETLINE(&line, &line_len, file)) != -1) {
    // Remove trailing newline if present
    if (read > 0 && line[read - 1] == '\n') {
      read--;
      line[read] = '\0';
    }

    KronosValue *line_val = value_new_string(line, (size_t)read);
    if (!line_val) {
      free(line);
      fclose(file);
      value_release(result);
      value_release(path_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(line_val);
        free(line);
        fclose(file);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    value_retain(line_val);
    result->as.list.items[result->as.list.count++] = line_val;
    value_release(line_val);
  }

  if (ferror(file) || !feof(file)) {
    free(line);
    fclose(file);
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Failed to read all lines from file '%s'",
                        path_arg->as.string.data);
    value_release(result);
    value_release(path_arg);
    return err;
  }

  free(line);
  fclose(file);
  value_release(path_arg);

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_file_exists(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'file_exists' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'file_exists' requires a string argument");
    value_release(path_arg);
    return err;
  }

  struct stat st;
  int exists = (stat(path_arg->as.string.data, &st) == 0);
  value_release(path_arg);

  KronosValue *result = value_new_bool(exists);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_list_files(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'list_files' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'list_files' requires a string argument");
    value_release(path_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    value_release(path_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

#ifdef _WIN32
  int path_wlen =
      MultiByteToWideChar(CP_UTF8, 0, path_arg->as.string.data, -1, NULL, 0);
  if (path_wlen <= 0) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
                        path_arg->as.string.data);
    value_release(result);
    value_release(path_arg);
    return err;
  }

  size_t pattern_cap = (size_t)path_wlen + 2;
  wchar_t *search_pattern = malloc(pattern_cap * sizeof(wchar_t));
  if (!search_pattern) {
    value_release(result);
    value_release(path_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate path buffer");
  }

  if (MultiByteToWideChar(CP_UTF8, 0, path_arg->as.string.data, -1,
                          search_pattern, path_wlen) <= 0) {
    free(search_pattern);
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
                        path_arg->as.string.data);
    value_release(result);
    value_release(path_arg);
    return err;
  }

  size_t pattern_len = (size_t)path_wlen - 1;
  if (pattern_len == 0 ||
      (search_pattern[pattern_len - 1] != L'\\' &&
       search_pattern[pattern_len - 1] != L'/')) {
    search_pattern[pattern_len++] = L'\\';
  }
  search_pattern[pattern_len++] = L'*';
  search_pattern[pattern_len] = L'\0';

  WIN32_FIND_DATAW find_data;
  HANDLE find_handle = FindFirstFileW(search_pattern, &find_data);
  free(search_pattern);

  if (find_handle != INVALID_HANDLE_VALUE) {
    do {
      if ((find_data.cFileName[0] == L'.' && find_data.cFileName[1] == L'\0') ||
          (find_data.cFileName[0] == L'.' && find_data.cFileName[1] == L'.' &&
           find_data.cFileName[2] == L'\0')) {
        continue;
      }

      int name_len = WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1,
                                         NULL, 0, NULL, NULL);
      if (name_len <= 0) {
        FindClose(find_handle);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to convert filename to UTF-8");
      }

      char *name_utf8 = malloc((size_t)name_len);
      if (!name_utf8) {
        FindClose(find_handle);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate filename");
      }

      if (WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, name_utf8,
                              name_len, NULL, NULL) <= 0) {
        free(name_utf8);
        FindClose(find_handle);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to convert filename to UTF-8");
      }

      KronosValue *name_val =
          value_new_string(name_utf8, (size_t)(name_len - 1));
      free(name_utf8);
      if (!name_val) {
        FindClose(find_handle);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
      }

      // Grow list if needed
      if (result->as.list.count >= result->as.list.capacity) {
        size_t old_cap = result->as.list.capacity;
        size_t new_cap = old_cap * 2;
        KronosValue **new_items =
            realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
        if (!new_items) {
          value_release(name_val);
          FindClose(find_handle);
          value_release(result);
          value_release(path_arg);
          return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
        }
        result->as.list.items = new_items;
        result->as.list.capacity = new_cap;
        // Initialize new slots to NULL (realloc doesn't zero new memory)
        memset(&new_items[old_cap], 0,
               (new_cap - old_cap) * sizeof(KronosValue *));
      }

      value_retain(name_val);
      result->as.list.items[result->as.list.count++] = name_val;
      value_release(name_val);
    } while (FindNextFileW(find_handle, &find_data) != 0);

    DWORD find_error = GetLastError();
    FindClose(find_handle);
    if (find_error != ERROR_NO_MORE_FILES) {
      int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                          "Failed to read directory '%s'",
                          path_arg->as.string.data);
      value_release(result);
      value_release(path_arg);
      return err;
    }
  } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
                        path_arg->as.string.data);
    value_release(result);
    value_release(path_arg);
    return err;
  }
#else
  DIR *dir = opendir(path_arg->as.string.data);
  if (!dir) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME, "Failed to open directory '%s'",
                        path_arg->as.string.data);
    value_release(result);
    value_release(path_arg);
    return err;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    // Skip . and ..
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    size_t name_len = strlen(entry->d_name);
    KronosValue *name_val = value_new_string(entry->d_name, name_len);
    if (!name_val) {
      closedir(dir);
      value_release(result);
      value_release(path_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t old_cap = result->as.list.capacity;
      size_t new_cap = old_cap * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(name_val);
        closedir(dir);
        value_release(result);
        value_release(path_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
      // Initialize new slots to NULL (realloc doesn't zero new memory)
      memset(&new_items[old_cap], 0,
             (new_cap - old_cap) * sizeof(KronosValue *));
    }

    value_retain(name_val);
    result->as.list.items[result->as.list.count++] = name_val;
    value_release(name_val);
  }

  closedir(dir);
#endif
  value_release(path_arg);

  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_join_path(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'join_path' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *path2_arg;

  POP_OR_RETURN(vm, path2_arg);
  KronosValue *path1_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, path1_arg, value_release(path2_arg));
  if (path1_arg->type != VAL_STRING || path2_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'join_path' requires two string arguments");
    value_release(path1_arg);
    value_release(path2_arg);
    return err;
  }

  const char *path1 = path1_arg->as.string.data;
  const char *path2 = path2_arg->as.string.data;
  size_t path1_len = path1_arg->as.string.length;
  size_t path2_len = path2_arg->as.string.length;
  size_t path2_start = 0;
  int path1_has_trailing_sep =
      path1_len > 0 && is_path_separator(path1[path1_len - 1]);
  char separator =
      (path1_has_trailing_sep && path1[path1_len - 1] == '\\') ? '\\' : '/';

  if (path1_has_trailing_sep && path2_len > 0 && is_path_separator(path2[0])) {
    while (path2_start < path2_len && is_path_separator(path2[path2_start])) {
      path2_start++;
    }
  }
  size_t path2_copy_len = path2_len - path2_start;
  int path2_has_leading_sep =
      path2_copy_len > 0 && is_path_separator(path2[path2_start]);

  // Calculate result length
  size_t result_len = path1_len + path2_copy_len + 1; // +1 for separator
  char *joined = malloc(result_len + 1);
  if (!joined) {
    value_release(path1_arg);
    value_release(path2_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to allocate memory");
  }

  // Copy first path
  memcpy(joined, path1, path1_len);
  size_t offset = path1_len;

  // Add separator if needed
  if (path1_len > 0 && !path1_has_trailing_sep && path2_copy_len > 0 &&
      !path2_has_leading_sep) {
    joined[offset++] = separator;
  }

  // Copy second path
  memcpy(joined + offset, path2 + path2_start, path2_copy_len);
  offset += path2_copy_len;
  joined[offset] = '\0';

  KronosValue *result = value_new_string(joined, offset);
  free(joined);
  value_release(path1_arg);
  value_release(path2_arg);

  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_dirname(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'dirname' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'dirname' requires a string argument");
    value_release(path_arg);
    return err;
  }

  const char *path = path_arg->as.string.data;
  size_t path_len = path_arg->as.string.length;
  size_t end = path_len;
  int has_drive_root = path_len >= 2 && isalpha((unsigned char)path[0]) &&
                       path[1] == ':';

  // Trim trailing separators.
  while (end > 0 && is_path_separator(path[end - 1])) {
    end--;
  }

  if (end == 0 && path_len > 0) {
    char root_sep = (path[0] == '\\') ? '\\' : '/';
    KronosValue *result = value_new_string(&root_sep, 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // Find last separator in the trimmed path.
  size_t last_sep = end;
  for (size_t i = end; i > 0; i--) {
    if (is_path_separator(path[i - 1])) {
      last_sep = i - 1;
      break;
    }
  }

  // If no separator found, return "."
  if (last_sep == end) {
    if (has_drive_root && end == 2 && path_len > end &&
        is_path_separator(path[end])) {
      KronosValue *result = value_new_string(path, end + 1);
      value_release(path_arg);
      if (!result) {
        return vm_error(vm, KRONOS_ERR_INTERNAL,
                        "Failed to create string value");
      }
      PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
      value_release(result);
      return 0;
    }
    KronosValue *result = value_new_string(".", 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  size_t dir_len = last_sep;
  while (dir_len > 0 && is_path_separator(path[dir_len - 1])) {
    dir_len--;
  }
  size_t dir_result_len = dir_len;
  if (has_drive_root && dir_len == 2 && path_len > dir_len &&
      is_path_separator(path[dir_len])) {
    dir_result_len++;
  }

  // If separator is at start, return root separator.
  if (dir_len == 0) {
    char root_sep = (path[0] == '\\') ? '\\' : '/';
    KronosValue *result = value_new_string(&root_sep, 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // Return path up to (but not including) last separator
  KronosValue *result = value_new_string(path, dir_result_len);
  value_release(path_arg);
  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

int builtin_basename(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 1) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'basename' expects 1 argument, got %d",
                     arg_count);
  }
  KronosValue *path_arg;

  POP_OR_RETURN(vm, path_arg);
  if (path_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'basename' requires a string argument");
    value_release(path_arg);
    return err;
  }

  const char *path = path_arg->as.string.data;
  size_t path_len = path_arg->as.string.length;
  size_t end = path_len;
  int has_drive_root = path_len >= 2 && isalpha((unsigned char)path[0]) &&
                       path[1] == ':';

  // Trim trailing separators.
  while (end > 0 && is_path_separator(path[end - 1])) {
    end--;
  }

  if (end == 0 && path_len > 0) {
    char root_sep = (path[0] == '\\') ? '\\' : '/';
    KronosValue *result = value_new_string(&root_sep, 1);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // Find last separator in the trimmed path.
  size_t last_sep = end;
  for (size_t i = end; i > 0; i--) {
    if (is_path_separator(path[i - 1])) {
      last_sep = i - 1;
      break;
    }
  }

  // If no separator found, return entire path
  if (last_sep == end) {
    size_t base_len = end;
    if (has_drive_root && end == 2 && path_len > end &&
        is_path_separator(path[end])) {
      base_len++;
    }
    if (base_len == path_len) {
      value_retain(path_arg);
      PUSH_OR_RETURN_WITH_CLEANUP(vm, path_arg, value_release(path_arg););
      value_release(path_arg);
      return 0;
    }
    KronosValue *result = value_new_string(path, base_len);
    value_release(path_arg);
    if (!result) {
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }
    PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
    value_release(result);
    return 0;
  }

  // Return path after last separator
  size_t name_start = last_sep + 1;
  size_t name_len = end - name_start;
  KronosValue *result = value_new_string(path + name_start, name_len);
  value_release(path_arg);
  if (!result) {
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result););
  value_release(result);
  return 0;
}

#if !KRONOS_HAS_POSIX_REGEX
static int regex_unavailable_error(KronosVM *vm, const char *function_name,
                                   KronosValue *pattern_arg,
                                   KronosValue *string_arg) {
  int err = vm_errorf(
      vm, KRONOS_ERR_RUNTIME,
      "Function '%s' is unavailable: this build lacks POSIX regex support",
      function_name);
  value_release(pattern_arg);
  value_release(string_arg);
  return err;
}
#endif

int builtin_regex_match(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.match' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.match' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

#if KRONOS_HAS_POSIX_REGEX
  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  int match = regexec(&regex, string_arg->as.string.data, 0, NULL, 0) == 0;
  regfree(&regex);

  KronosValue *result = value_new_bool(match);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
#else
  return regex_unavailable_error(vm, "regex.match", pattern_arg, string_arg);
#endif
}

int builtin_regex_search(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.search' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.search' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

#if KRONOS_HAS_POSIX_REGEX
  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  regmatch_t match;
  int found = regexec(&regex, string_arg->as.string.data, 1, &match, 0) == 0;

  KronosValue *result;
  if (found && match.rm_so >= 0) {
    // Extract matched substring
    size_t match_len = (size_t)(match.rm_eo - match.rm_so);
    result =
        value_new_string(string_arg->as.string.data + match.rm_so, match_len);
  } else {
    // No match - return nil
    result = value_new_nil();
  }
  regfree(&regex);

  if (!result) {
    value_release(pattern_arg);
    value_release(string_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create result value");
  }
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
#else
  return regex_unavailable_error(vm, "regex.search", pattern_arg, string_arg);
#endif
}

int builtin_regex_findall(KronosVM *vm, uint8_t arg_count) {
  if (arg_count != 2) {
    return vm_errorf(vm, KRONOS_ERR_RUNTIME,
                     "Function 'regex.findall' expects 2 arguments, got %d",
                     arg_count);
  }
  KronosValue *pattern_arg;

  POP_OR_RETURN(vm, pattern_arg);
  KronosValue *string_arg;

  POP_OR_RETURN_WITH_CLEANUP(vm, string_arg, value_release(pattern_arg));
  if (pattern_arg->type != VAL_STRING || string_arg->type != VAL_STRING) {
    int err = vm_errorf(vm, KRONOS_ERR_RUNTIME,
                        "Function 'regex.findall' requires string arguments");
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

#if KRONOS_HAS_POSIX_REGEX
  regex_t regex;
  int ret = regcomp(&regex, pattern_arg->as.string.data, REG_EXTENDED);
  if (ret != 0) {
    // regcomp() failed - regex structure is in undefined state
    // regerror() is safe to call with the error code even after failed
    // regcomp() Do NOT call regfree() on a failed regcomp() - it's unsafe
    char errbuf[REGEX_ERROR_BUFFER_SIZE];
    regerror(ret, &regex, errbuf, sizeof(errbuf));
    int err =
        vm_errorf(vm, KRONOS_ERR_RUNTIME, "Invalid regex pattern: %s", errbuf);
    value_release(pattern_arg);
    value_release(string_arg);
    return err;
  }

  KronosValue *result = value_new_list(16);
  if (!result) {
    regfree(&regex);
    value_release(pattern_arg);
    value_release(string_arg);
    return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create list");
  }

  const char *search_str = string_arg->as.string.data;
  size_t search_len = string_arg->as.string.length;
  size_t offset = 0;
  regmatch_t match;

  while (offset < search_len) {
    int found = regexec(&regex, search_str + offset, 1, &match, 0) == 0;
    if (!found || match.rm_so < 0) {
      break;
    }

    // Adjust match positions to absolute offsets
    size_t match_start = offset + (size_t)match.rm_so;
    size_t match_end = offset + (size_t)match.rm_eo;
    size_t match_len = match_end - match_start;

    // Extract matched substring
    KronosValue *match_val =
        value_new_string(search_str + match_start, match_len);
    if (!match_val) {
      regfree(&regex);
      value_release(result);
      value_release(pattern_arg);
      value_release(string_arg);
      return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to create string value");
    }

    // Grow list if needed
    if (result->as.list.count >= result->as.list.capacity) {
      size_t new_cap = result->as.list.capacity * 2;
      KronosValue **new_items =
          realloc(result->as.list.items, sizeof(KronosValue *) * new_cap);
      if (!new_items) {
        value_release(match_val);
        regfree(&regex);
        value_release(result);
        value_release(pattern_arg);
        value_release(string_arg);
        return vm_error(vm, KRONOS_ERR_INTERNAL, "Failed to grow list");
      }
      result->as.list.items = new_items;
      result->as.list.capacity = new_cap;
    }

    value_retain(match_val);
    result->as.list.items[result->as.list.count++] = match_val;
    value_release(match_val);

    // Move offset past this match
    if (match.rm_eo > match.rm_so) {
      offset = match_end;
    } else {
      // Zero-length match - advance by one character to avoid infinite loop
      offset++;
    }
  }

  regfree(&regex);
  PUSH_OR_RETURN_WITH_CLEANUP(vm, result, value_release(result);
                              value_release(pattern_arg);
                              value_release(string_arg););
  value_release(result);
  value_release(pattern_arg);
  value_release(string_arg);
  return 0;
#else
  return regex_unavailable_error(vm, "regex.findall", pattern_arg, string_arg);
#endif
}
