#!/usr/bin/env python3
"""
Fix gperf-generated keywords_hash.c by removing problematic preprocessor directives.
Removes #ifdef __GNUC__ blocks and related inline keyword directives.
"""

import sys

def fix_keywords_hash(input_file):
    with open(input_file, 'r') as f:
        lines = f.readlines()
    
    result = []
    skip_depth = 0
    
    for i, line in enumerate(lines):
        # Track nested #ifdef/#endif pairs
        if '#ifdef __GNUC__' in line:
            skip_depth = 1
            continue
        elif skip_depth > 0:
            # Count nested #ifdef/#if/#ifndef
            if '#ifdef' in line or ('#if' in line and not line.strip().startswith('#ifndef')):
                skip_depth += 1
            # Count #endif to close blocks
            elif '#endif' in line:
                skip_depth -= 1
                if skip_depth == 0:
                    # This is the closing #endif for __GNUC__, skip it too
                    continue
            # Skip all lines within the block
            continue
        # Remove __inline keywords
        elif '__inline' in line:
            continue
        # Remove standalone 'inline' that follows #else or #ifdef __cplusplus
        elif line.strip() == 'inline' and i > 0:
            prev_line = lines[i-1]
            if '#else' in prev_line or '#ifdef __cplusplus' in prev_line:
                continue
        
        result.append(line)
    
    with open(input_file, 'w') as f:
        f.write(''.join(result))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <keywords_hash.c>", file=sys.stderr)
        sys.exit(1)
    
    fix_keywords_hash(sys.argv[1])
