import re
import json
from pathlib import Path
from typing import List, Tuple

class Tokenizer:
    def __init__(self, code: str):
        self.tokens: List[Tuple[str, any]] = []
        self.patterns = self._load_patterns()
        self.tokenize(code)
        self.pos = 0
    
    def _load_patterns(self) -> List[Tuple[str, str]]:
        config_path = Path(__file__).parent / 'config' / 'tokens.json'
        with open(config_path, 'r') as f:
            config = json.load(f)
        
        patterns = []
        # Order matters: literals before keywords, keywords before identifiers
        for category in ['literals', 'keywords', 'identifiers', 
                        'punctuation', 'whitespace']:
            if category in config:
                patterns.extend(config[category])
        
        return patterns
    
    def tokenize(self, code: str):
        lines = code.split('\n')
        for line in lines:
            col = 0
            indent = len(line) - len(line.lstrip())
            if line.strip():
                self.tokens.append(('INDENT', indent))
            
            line = line.strip()
            while col < len(line):
                matched = False
                for token_type, pattern in self.patterns:
                    regex = re.compile(pattern)
                    match = regex.match(line, col)
                    if match:
                        text = match.group(0)
                        if token_type != 'SKIP':
                            self.tokens.append((token_type, text))
                        col = match.end()
                        matched = True
                        break
                if not matched:
                    col += 1
            
            if line:
                self.tokens.append(('NEWLINE', '\n'))