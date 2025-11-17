#include "gc.h"
#include <stdlib.h>
#include <string.h>

// Simple GC tracking for reference counting
#define MAX_TRACKED_OBJECTS 10000

typedef struct {
    KronosValue* objects[MAX_TRACKED_OBJECTS];
    size_t count;
    size_t allocated_bytes;
} GCState;

static GCState gc_state = {0};

// Initialize GC
void gc_init(void) {
    memset(&gc_state, 0, sizeof(GCState));
}

// Cleanup GC
void gc_cleanup(void) {
    // All objects should have been freed by now
    // This is just a safety check
    for (size_t i = 0; i < gc_state.count; i++) {
        if (gc_state.objects[i]) {
            // Force free leaked objects
            free(gc_state.objects[i]);
        }
    }
    gc_state.count = 0;
    gc_state.allocated_bytes = 0;
}

// Track a new object
void gc_track(KronosValue* val) {
    if (!val) return;

    if (gc_state.count < MAX_TRACKED_OBJECTS) {
        gc_state.objects[gc_state.count++] = val;
        gc_state.allocated_bytes += sizeof(KronosValue);

        // Add size of string data if applicable
        if (val->type == VAL_STRING) {
            gc_state.allocated_bytes += val->as.string.length + 1;
        }
    }
}

// Untrack an object (when it's being freed)
void gc_untrack(KronosValue* val) {
    if (!val) return;

    for (size_t i = 0; i < gc_state.count; i++) {
        if (gc_state.objects[i] == val) {
            // Subtract from allocated bytes
            gc_state.allocated_bytes -= sizeof(KronosValue);
            if (val->type == VAL_STRING) {
                gc_state.allocated_bytes -= val->as.string.length + 1;
            }

            // Remove from array by shifting
            for (size_t j = i; j < gc_state.count - 1; j++) {
                gc_state.objects[j] = gc_state.objects[j + 1];
            }
            gc_state.count--;
            return;
        }
    }
}

// Cycle detection using mark-and-sweep
// For now, this is a placeholder since we're focusing on reference counting
// Full cycle detection will be implemented when we have complex data structures
void gc_collect_cycles(void) {
    // TODO: Implement mark-and-sweep for cycles
    // This will be needed when we have circular references
    // (e.g., lists containing themselves, closures with circular refs)

    // For now, we rely on pure reference counting
}

// Get allocated bytes
size_t gc_get_allocated_bytes(void) {
    return gc_state.allocated_bytes;
}

// Get object count
size_t gc_get_object_count(void) {
    return gc_state.count;
}

