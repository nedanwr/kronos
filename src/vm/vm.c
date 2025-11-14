#include "vm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Create new VM
KronosVM* vm_new(void) {
    KronosVM* vm = malloc(sizeof(KronosVM));
    if (!vm) return NULL;
    
    vm->stack_top = vm->stack;
    vm->global_count = 0;
    vm->ip = NULL;
    vm->bytecode = NULL;
    
    return vm;
}

// Free VM
void vm_free(KronosVM* vm) {
    if (!vm) return;
    
    // Release all values on stack
    while (vm->stack_top > vm->stack) {
        vm->stack_top--;
        value_release(*vm->stack_top);
    }
    
    // Release global variables
    for (size_t i = 0; i < vm->global_count; i++) {
        free(vm->globals[i].name);
        value_release(vm->globals[i].value);
    }
    
    free(vm);
}

// Stack operations
static void push(KronosVM* vm, KronosValue* value) {
    if (vm->stack_top >= vm->stack + STACK_MAX) {
        fprintf(stderr, "Stack overflow\n");
        return;
    }
    *vm->stack_top = value;
    vm->stack_top++;
    value_retain(value); // Retain while on stack
}

static KronosValue* pop(KronosVM* vm) {
    if (vm->stack_top <= vm->stack) {
        fprintf(stderr, "Stack underflow\n");
        return value_new_nil();
    }
    vm->stack_top--;
    KronosValue* val = *vm->stack_top;
    return val;
}

static KronosValue* peek(KronosVM* vm, int distance) {
    return vm->stack_top[-1 - distance];
}

// Global variable management
void vm_set_global(KronosVM* vm, const char* name, KronosValue* value) {
    // Check if variable already exists
    for (size_t i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->globals[i].name, name) == 0) {
            value_release(vm->globals[i].value);
            vm->globals[i].value = value;
            value_retain(value);
            return;
        }
    }
    
    // Add new global
    if (vm->global_count >= GLOBALS_MAX) {
        fprintf(stderr, "Too many global variables\n");
        return;
    }
    
    vm->globals[vm->global_count].name = strdup(name);
    vm->globals[vm->global_count].value = value;
    value_retain(value);
    vm->global_count++;
}

KronosValue* vm_get_global(KronosVM* vm, const char* name) {
    for (size_t i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->globals[i].name, name) == 0) {
            return vm->globals[i].value;
        }
    }
    
    fprintf(stderr, "Undefined variable: %s\n", name);
    return value_new_nil();
}

// Read byte from bytecode
static uint8_t read_byte(KronosVM* vm) {
    return *vm->ip++;
}

// Read constant from pool
static KronosValue* read_constant(KronosVM* vm) {
    uint8_t idx = read_byte(vm);
    return vm->bytecode->constants[idx];
}

// Execute bytecode
int vm_execute(KronosVM* vm, Bytecode* bytecode) {
    if (!vm || !bytecode) return -1;
    
    vm->bytecode = bytecode;
    vm->ip = bytecode->code;
    
    while (1) {
        uint8_t instruction = read_byte(vm);
        
        switch (instruction) {
            case OP_LOAD_CONST: {
                KronosValue* constant = read_constant(vm);
                push(vm, constant);
                break;
            }
            
            case OP_LOAD_VAR: {
                KronosValue* name_val = read_constant(vm);
                if (name_val->type != VAL_STRING) {
                    fprintf(stderr, "Variable name must be string\n");
                    return -1;
                }
                KronosValue* value = vm_get_global(vm, name_val->as.string.data);
                push(vm, value);
                break;
            }
            
            case OP_STORE_VAR: {
                KronosValue* name_val = read_constant(vm);
                if (name_val->type != VAL_STRING) {
                    fprintf(stderr, "Variable name must be string\n");
                    return -1;
                }
                KronosValue* value = pop(vm);
                vm_set_global(vm, name_val->as.string.data, value);
                value_release(value); // Release our reference
                break;
            }
            
            case OP_PRINT: {
                KronosValue* value = pop(vm);
                value_print(value);
                printf("\n");
                value_release(value);
                break;
            }
            
            case OP_ADD: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    KronosValue* result = value_new_number(a->as.number + b->as.number);
                    push(vm, result);
                    value_release(result); // Push retains it
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_SUB: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    KronosValue* result = value_new_number(a->as.number - b->as.number);
                    push(vm, result);
                    value_release(result);
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_MUL: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    KronosValue* result = value_new_number(a->as.number * b->as.number);
                    push(vm, result);
                    value_release(result);
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_DIV: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    if (b->as.number == 0) {
                        fprintf(stderr, "Division by zero\n");
                        value_release(a);
                        value_release(b);
                        return -1;
                    }
                    KronosValue* result = value_new_number(a->as.number / b->as.number);
                    push(vm, result);
                    value_release(result);
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_EQ: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                bool result = value_equals(a, b);
                push(vm, value_new_bool(result));
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_NEQ: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                bool result = !value_equals(a, b);
                push(vm, value_new_bool(result));
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_GT: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    bool result = a->as.number > b->as.number;
                    push(vm, value_new_bool(result));
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_LT: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    bool result = a->as.number < b->as.number;
                    push(vm, value_new_bool(result));
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_GTE: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    bool result = a->as.number >= b->as.number;
                    push(vm, value_new_bool(result));
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_LTE: {
                KronosValue* b = pop(vm);
                KronosValue* a = pop(vm);
                
                if (a->type == VAL_NUMBER && b->type == VAL_NUMBER) {
                    bool result = a->as.number <= b->as.number;
                    push(vm, value_new_bool(result));
                } else {
                    fprintf(stderr, "Operands must be numbers\n");
                }
                
                value_release(a);
                value_release(b);
                break;
            }
            
            case OP_JUMP: {
                int8_t offset = (int8_t)read_byte(vm);
                vm->ip += offset;
                break;
            }
            
            case OP_JUMP_IF_FALSE: {
                uint8_t offset = read_byte(vm);
                KronosValue* condition = peek(vm, 0);
                if (!value_is_truthy(condition)) {
                    vm->ip += offset;
                }
                value_release(pop(vm)); // Pop condition
                break;
            }
            
            case OP_POP: {
                value_release(pop(vm));
                break;
            }
            
            case OP_HALT: {
                return 0;
            }
            
            default:
                fprintf(stderr, "Unknown instruction: %d\n", instruction);
                return -1;
        }
    }
    
    return 0;
}

