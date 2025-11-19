import re
from typing import List, Tuple

class Tokenizer:
    def __init__(self, code: str):
        self.tokens: List[Tuple[str, any]] = []
        self.tokenize(code)
        self.pos = 0
    
    def tokenize(self, code: str):
        patterns = [
            ('NUMBER', r'\d+(\.\d+)?'),
            ('STRING', r'"[^"]*"'),
            ('SET', r'\bset\b'),
            ('TO', r'\bto\b'),
            ('IF', r'\bif\b'),
            ('FOR', r'\bfor\b'),
            ('WHILE', r'\bwhile\b'),
            ('IN', r'\bin\b'),
            ('RANGE', r'\brange\b'),
            ('FUNCTION', r'\bfunction\b'),
            ('WITH', r'\bwith\b'),
            ('CALL', r'\bcall\b'),
            ('RETURN', r'\breturn\b'),
            ('IS', r'\bis\b'),
            ('EQUAL', r'\bequal\b'),
            ('NOT', r'\bnot\b'),
            ('GREATER', r'\bgreater\b'),
            ('LESS', r'\bless\b'),
            ('THAN', r'\bthan\b'),
            ('AND', r'\band\b'),
            ('OR', r'\bor\b'),
            ('PRINT', r'\bprint\b'),
            ('PLUS', r'\bplus\b'),
            ('MINUS', r'\bminus\b'),
            ('TIMES', r'\btimes\b'),
            ('DIVIDED', r'\bdivided\b'),
            ('BY', r'\bby\b'),
            ('NAME', r'[a-zA-Z_]\w*'),
            ('COLON', r':'),
            ('COMMA', r','),
            ('NEWLINE', r'\n'),
            ('INDENT', r'^[ \t]+'),
            ('SKIP', r'[ \t]+'),
        ]
        
        lines = code.split('\n')
        for line in lines:
            col = 0
            indent = len(line) - len(line.lstrip())
            if line.strip():
                self.tokens.append(('INDENT', indent))
            
            line = line.strip()
            while col < len(line):
                matched = False
                for token_type, pattern in patterns:
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