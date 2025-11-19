# Archived Python Implementation

This directory contains the original Python implementation of Kronos that was used as the reference for the C implementation.

## Original Files
- `main.py` - Original Python entry point
- `tokenizer.py` - Python tokenizer
- `parser.py` - Python parser  
- `interpreter.py` - Python interpreter
- `__init__.py` - Python package initialization

The C implementation maintains the same syntax and behavior while providing:
- Faster execution (bytecode VM)
- Lower memory usage (reference counting GC)
- Direct execution without Python runtime
- Production-ready performance

