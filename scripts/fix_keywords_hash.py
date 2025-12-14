#!/usr/bin/env python3
"""
Fix gperf-generated keywords_hash.c by removing problematic preprocessor directives.
Removes #ifdef __GNUC__ blocks and related inline keyword directives.
Preserves hash function definitions even if they're inside __GNUC__ blocks.
"""

import sys
import re

def fix_keywords_hash(input_file):
    with open(input_file, 'r') as f:
        lines = f.readlines()
    
    result = []
    skip_depth = 0
    hash_function_lines = []  # Store hash function if found in __GNUC__ block
    collecting_hash = False
    brace_count = 0
    
    i = 0
    while i < len(lines):
        line = lines[i]
        
        # Track nested #ifdef/#endif pairs
        if '#ifdef __GNUC__' in line:
            skip_depth = 1
            # Look ahead to see if there's a hash function in this block
            j = i + 1
            temp_depth = 1
            temp_hash_lines = []
            temp_brace_count = 0
            temp_collecting = False
            
            while j < len(lines) and temp_depth > 0:
                check_line = lines[j]
                if '#ifdef' in check_line or ('#if' in check_line and not check_line.strip().startswith('#ifndef')):
                    temp_depth += 1
                elif '#endif' in check_line:
                    temp_depth -= 1
                    if temp_depth == 0:
                        break
                
                # Check if this line starts a hash function
                if re.search(r'\bhash\s*\(', check_line):
                    temp_collecting = True
                    temp_hash_lines.append(check_line)
                    temp_brace_count += check_line.count('{') - check_line.count('}')
                elif temp_collecting:
                    temp_hash_lines.append(check_line)
                    temp_brace_count += check_line.count('{') - check_line.count('}')
                    if temp_brace_count == 0 and '{' in ''.join(temp_hash_lines):
                        # Found complete hash function
                        hash_function_lines = temp_hash_lines
                        collecting_hash = True
                        break
                j += 1
            
            i += 1
            continue
        elif skip_depth > 0:
            # Count nested #ifdef/#if/#ifndef
            if '#ifdef' in line or ('#if' in line and not line.strip().startswith('#ifndef')):
                skip_depth += 1
            # Count #endif to close blocks
            elif '#endif' in line:
                skip_depth -= 1
                if skip_depth == 0:
                    # This is the closing #endif for __GNUC__
                    # If we collected a hash function, add it now (without inline/__GNUC__ wrapper)
                    if hash_function_lines:
                        # Remove inline keywords from the function
                        for hline in hash_function_lines:
                            if '__inline' not in hline and hline.strip() != 'inline':
                                result.append(hline)
                        hash_function_lines = []
                        collecting_hash = False
                    i += 1
                    continue
            # Skip all lines within the block (unless we're collecting hash function)
            if not collecting_hash:
                i += 1
                continue
        # Remove __inline keywords
        elif '__inline' in line:
            i += 1
            continue
        # Remove standalone 'inline' that follows #else or #ifdef __cplusplus
        elif line.strip() == 'inline' and i > 0:
            prev_line = lines[i-1]
            if '#else' in prev_line or '#ifdef __cplusplus' in prev_line:
                i += 1
                continue
        
        result.append(line)
        i += 1
    
    content = ''.join(result)
    
    # Rename hash function to keyword_hash_func
    # This is needed because sed commands might not match all formats (e.g., when
    # the function declaration is split across multiple lines). We do a simple
    # rename here, and the sed commands will handle converting old-style to modern C.
    # Replace hash( with keyword_hash_func( - this handles both definitions and calls
    content = re.sub(r'\bhash\s*\(', 'keyword_hash_func(', content)
    
    with open(input_file, 'w') as f:
        f.write(content)

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <keywords_hash.c>", file=sys.stderr)
        sys.exit(1)
    
    fix_keywords_hash(sys.argv[1])
