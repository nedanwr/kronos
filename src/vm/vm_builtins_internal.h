#ifndef KRONOS_VM_BUILTINS_INTERNAL_H
#define KRONOS_VM_BUILTINS_INTERNAL_H

#include "vm.h"
#include <stdint.h>

int builtin_read_file(KronosVM *vm, uint8_t arg_count);
int builtin_add(KronosVM *vm, uint8_t arg_count);
int builtin_subtract(KronosVM *vm, uint8_t arg_count);
int builtin_multiply(KronosVM *vm, uint8_t arg_count);
int builtin_divide(KronosVM *vm, uint8_t arg_count);
int builtin_len(KronosVM *vm, uint8_t arg_count);
int builtin_uppercase(KronosVM *vm, uint8_t arg_count);
int builtin_lowercase(KronosVM *vm, uint8_t arg_count);
int builtin_trim(KronosVM *vm, uint8_t arg_count);
int builtin_split(KronosVM *vm, uint8_t arg_count);
int builtin_join(KronosVM *vm, uint8_t arg_count);
int builtin_to_string(KronosVM *vm, uint8_t arg_count);
int builtin_contains(KronosVM *vm, uint8_t arg_count);
int builtin_starts_with(KronosVM *vm, uint8_t arg_count);
int builtin_ends_with(KronosVM *vm, uint8_t arg_count);
int builtin_replace(KronosVM *vm, uint8_t arg_count);
int builtin_sqrt(KronosVM *vm, uint8_t arg_count);
int builtin_power(KronosVM *vm, uint8_t arg_count);
int builtin_abs(KronosVM *vm, uint8_t arg_count);
int builtin_round(KronosVM *vm, uint8_t arg_count);
int builtin_floor(KronosVM *vm, uint8_t arg_count);
int builtin_ceil(KronosVM *vm, uint8_t arg_count);
int builtin_rand(KronosVM *vm, uint8_t arg_count);
int builtin_min(KronosVM *vm, uint8_t arg_count);
int builtin_max(KronosVM *vm, uint8_t arg_count);
int builtin_to_number(KronosVM *vm, uint8_t arg_count);
int builtin_to_bool(KronosVM *vm, uint8_t arg_count);
int builtin_reverse(KronosVM *vm, uint8_t arg_count);
int builtin_sort(KronosVM *vm, uint8_t arg_count);
int builtin_filter(KronosVM *vm, uint8_t arg_count);
int builtin_map(KronosVM *vm, uint8_t arg_count);
int builtin_write_file(KronosVM *vm, uint8_t arg_count);
int builtin_read_lines(KronosVM *vm, uint8_t arg_count);
int builtin_file_exists(KronosVM *vm, uint8_t arg_count);
int builtin_list_files(KronosVM *vm, uint8_t arg_count);
int builtin_join_path(KronosVM *vm, uint8_t arg_count);
int builtin_dirname(KronosVM *vm, uint8_t arg_count);
int builtin_basename(KronosVM *vm, uint8_t arg_count);
int builtin_regex_match(KronosVM *vm, uint8_t arg_count);
int builtin_regex_search(KronosVM *vm, uint8_t arg_count);
int builtin_regex_findall(KronosVM *vm, uint8_t arg_count);

#endif
