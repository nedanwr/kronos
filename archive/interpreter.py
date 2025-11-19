from typing import Any, Dict, List, Tuple

class Interpreter:
    def __init__(self):
        self.variables: Dict[str, Any] = {}
        self.functions: Dict[str, Tuple[List[str], List]] = {}
        self.return_value = None
    
    def run(self, statements: List[Tuple]):
        for stmt in statements:
            result = self.execute(stmt)
            if result == 'return':
                break
    
    def execute(self, stmt: Tuple) -> str:
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
    
    def call_function(self, name: str, args: List[Tuple]) -> Any:
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
    
    def eval_condition(self, condition: Tuple) -> bool:
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
    
    def eval_expression(self, expr: Tuple) -> Any:
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