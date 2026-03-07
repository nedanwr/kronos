#include "vm_builtins_registry.h"
#include "vm_builtins_internal.h"
#include <stdlib.h>
#include <string.h>

// Built-in function dispatch table entry
typedef struct {
  const char *name;
  BuiltinHandler handler;
} BuiltinEntry;

// Comparison function for binary search in builtin dispatch table
static int builtin_compare(const void *a, const void *b) {
  const BuiltinEntry *entry_a = (const BuiltinEntry *)a;
  const BuiltinEntry *entry_b = (const BuiltinEntry *)b;
  return strcmp(entry_a->name, entry_b->name);
}

// Built-in function dispatch table (sorted alphabetically for binary search)
static const BuiltinEntry builtin_table[] = {
    {"abs", builtin_abs},
    {"add", builtin_add},
    {"basename", builtin_basename},
    {"ceil", builtin_ceil},
    {"contains", builtin_contains},
    {"dirname", builtin_dirname},
    {"divide", builtin_divide},
    {"ends_with", builtin_ends_with},
    {"file_exists", builtin_file_exists},
    {"filter", builtin_filter},
    {"findall", builtin_regex_findall},
    {"floor", builtin_floor},
    {"join", builtin_join},
    {"join_path", builtin_join_path},
    {"len", builtin_len},
    {"list_files", builtin_list_files},
    {"lowercase", builtin_lowercase},
    {"map", builtin_map},
    {"match", builtin_regex_match},
    {"max", builtin_max},
    {"min", builtin_min},
    {"multiply", builtin_multiply},
    {"power", builtin_power},
    {"rand", builtin_rand},
    {"read_file", builtin_read_file},
    {"read_lines", builtin_read_lines},
    {"replace", builtin_replace},
    {"reverse", builtin_reverse},
    {"round", builtin_round},
    {"search", builtin_regex_search},
    {"sort", builtin_sort},
    {"split", builtin_split},
    {"sqrt", builtin_sqrt},
    {"starts_with", builtin_starts_with},
    {"subtract", builtin_subtract},
    {"to_bool", builtin_to_bool},
    {"to_number", builtin_to_number},
    {"to_string", builtin_to_string},
    {"trim", builtin_trim},
    {"uppercase", builtin_uppercase},
    {"write_file", builtin_write_file},
};
static const size_t builtin_table_size =
    sizeof(builtin_table) / sizeof(builtin_table[0]);

// Look up built-in function by name using binary search
BuiltinHandler vm_find_builtin(const char *name) {
  BuiltinEntry key = {name, NULL};
  BuiltinEntry *result =
      (BuiltinEntry *)bsearch(&key, builtin_table, builtin_table_size,
                              sizeof(BuiltinEntry), builtin_compare);
  return result ? result->handler : NULL;
}
