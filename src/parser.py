from typing import List, Tuple, Optional

class Parser:
    def __init__(self, tokens: List[Tuple[str, any]]):
        self.tokens = tokens
        self.pos = 0
    
    def peek(self, offset: int = 0) -> Optional[Tuple[str, any]]:
        idx = self.pos + offset
        return self.tokens[idx] if idx < len(self.tokens) else None
    
    def consume(self, expected: Optional[str] = None) -> Tuple[str, any]:
        token = self.tokens[self.pos]
        if expected and token[0] != expected:
            raise SyntaxError(f"Expected {expected}, got {token[0]}")
        self.pos += 1
        return token
    
    def parse(self) -> List[Tuple]:
        statements = []
        while self.pos < len(self.tokens):
            if self.peek()[0] == 'NEWLINE':
                self.consume()
                continue
            stmt = self.parse_statement()
            if stmt:
                statements.append(stmt)
        return statements
    
    def parse_statement(self) -> Optional[Tuple]:
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
        elif token[0] == 'NAME' and self.peek()[1] == '__init__':
            return self.parse_init(indent)
        elif token[0] == 'CALL':
            return self.parse_call(indent)
        elif token[0] == 'RETURN':
            return self.parse_return(indent)
        elif token[0] == 'TRY':
            return self.parse_try(indent)
        elif token[0] == 'RAISE':
            return self.parse_raise(indent)
        return None
    
    def parse_assignment(self, indent: int) -> Tuple:
        self.consume('SET')
        name = self.consume('NAME')[1]
        self.consume('TO')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('assign', indent, name, value)
    
    def parse_print(self, indent: int) -> Tuple:
        self.consume('PRINT')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('print', indent, value)
    
    def parse_if(self, indent: int) -> Tuple:
        self.consume('IF')
        condition = self.parse_condition()
        self.consume('COLON')
        self.consume('NEWLINE')
        if_block = self.parse_block(indent)
        
        # Check for else if / else
        elif_blocks = []
        else_block = None
        
        while self.pos < len(self.tokens):
            if self.peek()[0] != 'INDENT':
                break
            next_indent = int(self.peek()[1])
            if next_indent != indent:
                break
            
            self.consume('INDENT')
            
            if self.peek()[0] == 'ELSE':
                self.consume('ELSE')
                
                # Check if it's "else if" or just "else"
                if self.peek()[0] == 'IF':
                    self.consume('IF')
                    condition = self.parse_condition()
                    self.consume('COLON')
                    self.consume('NEWLINE')
                    elif_block = self.parse_block(indent)
                    elif_blocks.append((condition, elif_block))
                else:
                    # Just "else"
                    self.consume('COLON')
                    self.consume('NEWLINE')
                    else_block = self.parse_block(indent)
                    break  # else must be last
            else:
                self.pos -= 1  # Put indent back
                break
        
        return ('if', indent, condition, if_block, elif_blocks, else_block)
    
    def parse_for(self, indent: int) -> Tuple:
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
    
    def parse_while(self, indent: int) -> Tuple:
        self.consume('WHILE')
        condition = self.parse_condition()
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('while', indent, condition, block)
    
    def parse_function(self, indent: int) -> Tuple:
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
    
    def parse_init(self, indent: int) -> Tuple:
        # Parse __init__ special function
        self.consume('NAME')  # consume __init__
        self.consume('COLON')
        self.consume('NEWLINE')
        block = self.parse_block(indent)
        return ('function', indent, '__init__', [], block)
    
    def parse_call(self, indent: int) -> Tuple:
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
    
    def parse_return(self, indent: int) -> Tuple:
        self.consume('RETURN')
        value = self.parse_expression()
        self.consume('NEWLINE')
        return ('return', indent, value)
    
    def parse_try(self, indent: int) -> Tuple:
        self.consume('TRY')
        self.consume('COLON')
        self.consume('NEWLINE')
        try_block = self.parse_block(indent)
        
        # Parse error handlers
        handlers = []
        while self.pos < len(self.tokens):
            if self.peek()[0] != 'INDENT':
                break
            next_indent = int(self.peek()[1])
            if next_indent != indent:
                break
            
            self.consume('INDENT')
            if self.peek()[0] != 'ON':
                self.pos -= 1  # Put indent back
                break
            
            self.consume('ON')
            
            # Check for specific error type (NAME token with specific values)
            error_type = None
            if self.peek()[0] == 'NAME':
                error_name = self.peek()[1]
                if error_name in ['math', 'name', 'type', 'value']:
                    self.consume('NAME')
                    error_type = error_name
                    if error_name == 'name':
                        error_type = 'name_error'
            
            self.consume('ERROR')
            
            # Check for "as variable"
            error_var = None
            if self.peek()[0] == 'AS':
                self.consume('AS')
                error_var = self.consume('NAME')[1]
            
            self.consume('COLON')
            self.consume('NEWLINE')
            handler_block = self.parse_block(indent)
            
            handlers.append((error_type, error_var, handler_block))
        
        return ('try', indent, try_block, handlers)
    
    def parse_raise(self, indent: int) -> Tuple:
        self.consume('RAISE')
        
        # Check for error type (NAME token with specific values)
        error_type = 'error'
        if self.peek()[0] == 'NAME':
            error_name = self.peek()[1]
            if error_name in ['math', 'name', 'type', 'value']:
                self.consume('NAME')
                error_type = error_name
                if error_name == 'name':
                    error_type = 'name_error'
        
        self.consume('ERROR')
        message = self.parse_expression()
        self.consume('NEWLINE')
        return ('raise', indent, error_type, message)
    
    def parse_condition(self) -> Tuple:
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
            raise SyntaxError(f"Unknown comparison: {op_token}")
        
        right = self.parse_expression()
        return ('condition', left, op, right)
    
    def parse_expression(self) -> Tuple:
        left = self.parse_value()
        
        while (self.peek() and 
               self.peek()[0] in ['PLUS', 'MINUS', 'TIMES', 'DIVIDED']):
            op_token = self.peek()[0]
            self.consume()
            
            if op_token == 'DIVIDED':
                self.consume('BY')
                op = '/'
            else:
                op = {'PLUS': '+', 'MINUS': '-', 'TIMES': '*'}[op_token]
            
            right = self.parse_value()
            left = ('binop', left, op, right)
        
        return left
    
    def parse_value(self) -> Tuple:
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
    
    def parse_block(self, parent_indent: int) -> List[Tuple]:
        block = []
        
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