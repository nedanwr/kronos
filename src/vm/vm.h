#ifndef KRONOS_VM_H
#define KRONOS_VM_H

#include "../compiler/compiler.h"
#include "../core/runtime.h"
#include <stddef.h>

#define STACK_MAX 1024
#define GLOBALS_MAX 256

// Virtual machine state
typedef struct KronosVM {
    // Stack
    KronosValue* stack[STACK_MAX];
    KronosValue** stack_top;
    
    // Global variables
    struct {
        char* name;
        KronosValue* value;
    } globals[GLOBALS_MAX];
    size_t global_count;
    
    // Instruction pointer
    uint8_t* ip;
    
    // Current bytecode
    Bytecode* bytecode;
} KronosVM;

// VM lifecycle
KronosVM* vm_new(void);
void vm_free(KronosVM* vm);

// Execute bytecode
int vm_execute(KronosVM* vm, Bytecode* bytecode);

// Variable management
void vm_set_global(KronosVM* vm, const char* name, KronosValue* value);
KronosValue* vm_get_global(KronosVM* vm, const char* name);

#endif // KRONOS_VM_H

