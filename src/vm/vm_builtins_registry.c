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
    {"acos", builtin_acos},
    {"add", builtin_add},
    {"all", builtin_all},
    {"any", builtin_any},
    {"args", builtin_args},
    {"asin", builtin_asin},
    {"atan", builtin_atan},
    {"basename", builtin_basename},
    {"capitalize", builtin_capitalize},
    {"cbrt", builtin_cbrt},
    {"ceil", builtin_ceil},
    {"contains", builtin_contains},
    {"cos", builtin_cos},
    {"count", builtin_count},
    {"dirname", builtin_dirname},
    {"divide", builtin_divide},
    {"ends_with", builtin_ends_with},
    {"enumerate", builtin_enumerate},
    {"env", builtin_env},
    {"exit", builtin_exit},
    {"exp", builtin_exp},
    {"file_exists", builtin_file_exists},
    {"filter", builtin_filter},
    {"find", builtin_find},
    {"findall", builtin_regex_findall},
    {"floor", builtin_floor},
    {"format_date", builtin_format_date},
    {"join", builtin_join},
    {"join_path", builtin_join_path},
    {"len", builtin_len},
    {"list_files", builtin_list_files},
    {"log", builtin_log},
    {"log10", builtin_log10},
    {"lowercase", builtin_lowercase},
    {"map", builtin_map},
    {"match", builtin_regex_match},
    {"max", builtin_max},
    {"min", builtin_min},
    {"multiply", builtin_multiply},
    {"now", builtin_now},
    {"parse_date", builtin_parse_date},
    {"parse_json", builtin_parse_json},
    {"power", builtin_power},
    {"rand", builtin_rand},
    {"read_file", builtin_read_file},
    {"read_lines", builtin_read_lines},
    {"replace", builtin_replace},
    {"reverse", builtin_reverse},
    {"rfind", builtin_rfind},
    {"round", builtin_round},
    {"search", builtin_regex_search},
    {"sin", builtin_sin},
    {"sleep", builtin_sleep},
    {"sort", builtin_sort},
    {"split", builtin_split},
    {"sqrt", builtin_sqrt},
    {"starts_with", builtin_starts_with},
    {"subtract", builtin_subtract},
    {"sum", builtin_sum},
    {"tan", builtin_tan},
    {"title", builtin_title},
    {"to_bool", builtin_to_bool},
    {"to_json", builtin_to_json},
    {"to_number", builtin_to_number},
    {"to_string", builtin_to_string},
    {"trim", builtin_trim},
    {"uppercase", builtin_uppercase},
    {"write_file", builtin_write_file},
    {"zip", builtin_zip},
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
