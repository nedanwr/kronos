import re
from typing import Any, Dict, List, Tuple

class Tokenizer:
    def __init__(self, code: str):
        self.tokens = []
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

class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0
    
    def peek(self, offset=0):
        idx = self.pos + offset
        return self.tokens[idx] if idx < len(self.tokens) else None
    
    def consume(self, expected=None):
        token = self.tokens[self.pos]
        if expected and token[0] != expected:
            raise SyntaxError(f"Expected {expected}, got {token[0]}")
        self.pos += 1
        return token
    
    def parse(self):
        statements = []
        while self.pos < len(self.tokens):
            if self.peek()[0] == 'NEWLINE':
                self.consume()
                continue
            stmt = self.parse_statement()
            if stmt:
                statements.append(stmt)
        return statements
    
    def parse_statement(self):
        indent = 0
        if self.peek()[0] == 'INDENT':
            indent = int(self.peek()[1])
            self.consume()
        
        token = self.peek()
        if not token:
            return None
            
        if token[0] == 'SET':
            return self.parse_assignment(indent)
        elif token[0] == 'PRINT':
            return self.parse_print(indent)
        elif token[0] == 'IF':
            return self.parse_if(indent)
        elif token[0] == 'FOR':
            return self.parse_for(indent)
        elif token[0] == 'WHILE':
            return self.parse_while(indent)
        elif token[0] == 'FUNCTION':
            return self.parse_function(indent)
        elif token[0] == 'CALL':
            return self.parse_call(indent)
        elif token[0] == 'RETURN':
            return self.parse_return(indent)
        return None
    
    def parse_assignment(self, indent):
        self.consume('SET')
        name = self.consume('NAME')[1]
        self.consume('TO')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('assign', indent, name, value)
    
    def parse_print(self, indent):
        self.consume('PRINT')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('print', indent, value)
    
    def parse_if(self, indent):
        self.consume('IF')
        condition = self.parse_condition()
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('if', indent, condition, block)
    
    def parse_for(self, indent):
        self.consume('FOR')
        var = self.consume('NAME')[1]
        self.consume('IN')
        self.consume('RANGE')
        
        start = self.parse_expression()
        self.consume('TO')
        end = self.parse_expression()
        
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('for', indent, var, start, end, block)
    
    def parse_while(self, indent):
        self.consume('WHILE')
        condition = self.parse_condition()
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('while', indent, condition, block)
    
    def parse_function(self, indent):
        self.consume('FUNCTION')
        name = self.consume('NAME')[1]
        
        params = []
        if self.peek()[0] == 'WITH':
            self.consume('WITH')
            params.append(self.consume('NAME')[1])
            while self.peek()[0] == 'COMMA':
                self.consume('COMMA')
                params.append(self.consume('NAME')[1])
        
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('function', indent, name, params, block)
    
    def parse_call(self, indent):
        self.consume('CALL')
        name = self.consume('NAME')[1]
        
        args = []
        if self.peek()[0] == 'WITH':
            self.consume('WITH')
            args.append(self.parse_expression())
            while self.peek()[0] == 'COMMA':
                self.consume('COMMA')
                args.append(self.parse_expression())
        
        self.consume('NEWLINE')
        return ('call', indent, name, args)
    
    def parse_return(self, indent):
        self.consume('RETURN')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('return', indent, value)
    
    def parse_condition(self):
        left = self.parse_expression()
        self.consume('IS')
        
        negated = False
        if self.peek()[0] == 'NOT':
            self.consume('NOT')
            negated = True
        
        op_token = self.peek()[0]
        if op_token == 'EQUAL':
            self.consume('EQUAL')
            op = '!=' if negated else '=='
        elif op_token == 'GREATER':
            self.consume('GREATER')
            self.consume('THAN')
            op = '<=' if negated else '>'
        elif op_token == 'LESS':
            self.consume('LESS')
            self.consume('THAN')
            op = '>=' if negated else '<'
        else:
            raise SyntaxError(f"Unknown comparison operator: {op_token}")
        
        right = self.parse_expression()
        return ('condition', left, op, right)
    
    def parse_expression(self):
        left = self.parse_value()
        
        while self.peek() and self.peek()[0] in ['PLUS', 'MINUS', 'TIMES', 'DIVIDED']:
            op_token = self.peek()[0]
            self.consume()
            
            if op_token == 'DIVIDED':
                self.consume('BY')
                op = '/'
            else:
                op = {
                    'PLUS': '+',
                    'MINUS': '-',
                    'TIMES': '*'
                }[op_token]
            
            right = self.parse_value()
            left = ('binop', left, op, right)
        
        return left
    
    def parse_value(self):
        token = self.peek()
        if token[0] == 'NUMBER':
            self.consume()
            return ('number', float(token[1]))
        elif token[0] == 'STRING':
            self.consume()
            return ('string', token[1].strip('"'))
        elif token[0] == 'NAME':
            name = self.consume()[1]
            return ('var', name)
        raise SyntaxError(f"Unexpected token: {token}")
    
    def parse_block(self, parent_indent):
        block = []
        block_indent = parent_indent + 4
        
        while self.pos < len(self.tokens):
            if self.peek()[0] != 'INDENT':
                break
            next_indent = int(self.peek()[1])
            if next_indent <= parent_indent:
                break
            stmt = self.parse_statement()
            if stmt:
                block.append(stmt)
        
        return block

class Interpreter:
    def __init__(self):
        self.variables: Dict[str, Any] = {}
        self.functions: Dict[str, Tuple[List[str], List]] = {}
        self.return_value = None
    
    def run(self, statements):
        for stmt in statements:
            result = self.execute(stmt)
            if result == 'return':
                break
    
    def execute(self, stmt):
        stmt_type = stmt[0]
        
        if stmt_type == 'assign':
            _, _, name, value = stmt
            self.variables[name] = self.eval_expression(value)
        
        elif stmt_type == 'print':
            _, _, value = stmt
            result = self.eval_expression(value)
            print(result)
        
        elif stmt_type == 'if':
            _, _, condition, block = stmt
            if self.eval_condition(condition):
                for block_stmt in block:
                    result = self.execute(block_stmt)
                    if result == 'return':
                        return 'return'
        
        elif stmt_type == 'for':
            _, _, var, start, end, block = stmt
            start_val = int(self.eval_expression(start))
            end_val = int(self.eval_expression(end))
            
            for i in range(start_val, end_val + 1):
                self.variables[var] = i
                for block_stmt in block:
                    result = self.execute(block_stmt)
                    if result == 'return':
                        return 'return'
        
        elif stmt_type == 'while':
            _, _, condition, block = stmt
            while self.eval_condition(condition):
                for block_stmt in block:
                    result = self.execute(block_stmt)
                    if result == 'return':
                        return 'return'
        
        elif stmt_type == 'function':
            _, _, name, params, block = stmt
            self.functions[name] = (params, block)
        
        elif stmt_type == 'call':
            _, _, name, args = stmt
            self.call_function(name, args)
        
        elif stmt_type == 'return':
            _, _, value = stmt
            self.return_value = self.eval_expression(value)
            return 'return'
    
    def call_function(self, name, args):
        if name not in self.functions:
            raise NameError(f"Function '{name}' not defined")
        
        params, block = self.functions[name]
        
        if len(args) != len(params):
            raise ValueError(
                f"Function '{name}' expects {len(params)} args, "
                f"got {len(args)}"
            )
        
        old_vars = self.variables.copy()
        
        for param, arg in zip(params, args):
            self.variables[param] = self.eval_expression(arg)
        
        self.return_value = None
        for stmt in block:
            result = self.execute(stmt)
            if result == 'return':
                break
        
        result = self.return_value
        self.variables = old_vars
        
        return result
    
    def eval_condition(self, condition):
        _, left, op, right = condition
        left_val = self.eval_expression(left)
        right_val = self.eval_expression(right)
        
        if op == '==':
            return left_val == right_val
        elif op == '!=':
            return left_val != right_val
        elif op == '>':
            return left_val > right_val
        elif op == '<':
            return left_val < right_val
        elif op == '>=':
            return left_val >= right_val
        elif op == '<=':
            return left_val <= right_val
    
    def eval_expression(self, expr):
        expr_type = expr[0]
        
        if expr_type == 'number':
            return expr[1]
        elif expr_type == 'string':
            return expr[1]
        elif expr_type == 'var':
            var_name = expr[1]
            if var_name not in self.variables:
                raise NameError(f"Variable '{var_name}' not defined")
            return self.variables[var_name]
        elif expr_type == 'binop':
            _, left, op, right = expr
            left_val = self.eval_expression(left)
            right_val = self.eval_expression(right)
            
            if op == '+':
                return left_val + right_val
            elif op == '-':
                return left_val - right_val
            elif op == '*':
                return left_val * right_val
            elif op == '/':
                return left_val / right_val

# Example usage
code = """
function greet with name:
    print "Hello"
    print name

call greet with "Alice"

function add with a, b:
    set sum to a plus b
    return sum

call add with 5, 3

function factorial with n:
    set result to 1
    for i in range 1 to n:
        set result to result times i
    return result

call factorial with 5

print "Done!"
"""

tokenizer = Tokenizer(code)
parser = Parser(tokenizer.tokens)
ast = parser.parse()
interpreter = Interpreter()
interpreter.run(ast)