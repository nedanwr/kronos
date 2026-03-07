#ifndef KRONOS_VM_BUILTINS_REGISTRY_H
#define KRONOS_VM_BUILTINS_REGISTRY_H

#include "vm_builtins.h"

BuiltinHandler vm_find_builtin(const char *name);

#endif
