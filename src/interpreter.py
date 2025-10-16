from typing import Any, Dict, List, Tuple, Set

class KronosError(Exception):
    def __init__(self, error_type: str, message: str):
        self.error_type = error_type
        self.message = message
        super().__init__(message)

class Interpreter:
    def __init__(self):
        self.variables: Dict[str, Any] = {}
        self.functions: Dict[str, Tuple[List[str], List]] = {}
        self.return_value = None
        self.defined_variables: Set[str] = set()
        self.constants: Set[str] = set()
        self.variable_types: Dict[str, str] = {}
    
    def run(self, statements: List[Tuple]):
        # First pass: collect all function definitions
        non_function_stmts = []
        has_init = False
        
        for stmt in statements:
            if stmt[0] == 'function':
                _, _, name, params, block = stmt
                self.functions[name] = (params, block)
                if name == '__init__':
                    has_init = True
            else:
                non_function_stmts.append(stmt)
        
        # Second pass: execute
        if has_init:
            # Execute __init__ as entry point
            if '__init__' not in self.functions:
                raise KronosError('name_error', '__init__ function not found')
            
            params, block = self.functions['__init__']
            if len(params) > 0:
                raise KronosError(
                    'value',
                    '__init__ should not have parameters'
                )
            
            for stmt in block:
                result = self.execute(stmt)
                if result == 'return':
                    break
        else:
            # Execute statements sequentially
            for stmt in non_function_stmts:
                result = self.execute(stmt)
                if result == 'return':
                    break
    
    def execute(self, stmt: Tuple) -> str:
        stmt_type = stmt[0]
        
        try:
            if stmt_type == 'assign':
                _, _, is_const, name, value, var_type = stmt

                # Check if trying to reassign a constant
                if name in self.constants:
                    raise KronosError(
                        'value',
                        f"Cannot reassign immutable variable '{name}'"
                    )
                
                evaluated_value = self.eval_expression(value)
            
                # Type checking if type annotation exists
                if var_type:
                    expected_type = var_type
                    actual_type = self.get_type(evaluated_value)

                    if actual_type != expected_type:
                        raise KronosError(
                            'type',
                            f"Type mismatch: '{name}' expects {expected_type}, "
                            f"got {actual_type}"
                        )
                    
                    # Store type constraint for future assignments
                    self.variable_types[name] = expected_type
                
                # If variable already has a type constraint, check if
                elif name in self.variable_types:
                    expected_type = self.variable_types[name]
                    actual_type = self.get_type(evaluated_value)

                    if actual_type != expected_type:
                        raise KronosError(
                            'type',
                            f"Type mismatch: '{name}' expects {expected_type}, "
                            f"got {actual_type}"
                        )

                self.variables[name] = evaluated_value
                self.defined_variables.add(name)

                # Mark as constant if using 'let'
                if is_const:
                    self.constants.add(name)

            elif stmt_type == 'print':
                _, _, value = stmt
                result = self.eval_expression(value)
                print(result)
            
            elif stmt_type == 'if':
                _, _, condition, if_block, elif_blocks, else_block = stmt
                
                if self.eval_condition(condition):
                    for block_stmt in if_block:
                        result = self.execute(block_stmt)
                        if result == 'return':
                            return 'return'
                else:
                    executed = False
                    for elif_condition, elif_block in elif_blocks:
                        if self.eval_condition(elif_condition):
                            for block_stmt in elif_block:
                                result = self.execute(block_stmt)
                                if result == 'return':
                                    return 'return'
                            executed = True
                            break
                    
                    if not executed and else_block:
                        for block_stmt in else_block:
                            result = self.execute(block_stmt)
                            if result == 'return':
                                return 'return'
            
            elif stmt_type == 'for':
                _, _, var, start, end, block = stmt
                start_val = int(self.eval_expression(start))
                end_val = int(self.eval_expression(end))
                
                for i in range(start_val, end_val + 1):
                    self.variables[var] = i
                    self.defined_variables.add(var)
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
                # Already handled in first pass
                pass
            
            elif stmt_type == 'call':
                _, _, name, args = stmt
                self.call_function(name, args)
            
            elif stmt_type == 'return':
                _, _, value = stmt
                self.return_value = self.eval_expression(value)
                return 'return'
            
            elif stmt_type == 'try':
                _, _, try_block, handlers = stmt
                self.execute_try(try_block, handlers)
            
            elif stmt_type == 'raise':
                _, _, error_type, message = stmt
                msg = self.eval_expression(message)
                raise KronosError(error_type, str(msg))
        
        except KronosError:
            raise
        except ZeroDivisionError as e:
            raise KronosError('math', 'Division by zero')
        except NameError as e:
            raise KronosError('name_error', str(e))
        except TypeError as e:
            raise KronosError('type', str(e))
        except ValueError as e:
            raise KronosError('value', str(e))
    
    def execute_try(self, try_block: List[Tuple], handlers: List[Tuple]):
        try:
            for stmt in try_block:
                result = self.execute(stmt)
                if result == 'return':
                    return 'return'
        except KronosError as e:
            for error_type, error_var, handler_block in handlers:
                if error_type is None or error_type == e.error_type:
                    if error_var:
                        old_val = self.variables.get(error_var)
                        self.variables[error_var] = e.message
                        self.defined_variables.add(error_var)
                    
                    for stmt in handler_block:
                        self.execute(stmt)
                    
                    if error_var:
                        if old_val is not None:
                            self.variables[error_var] = old_val
                        else:
                            del self.variables[error_var]
                            self.defined_variables.discard(error_var)
                    
                    return
            
            raise
    
    def call_function(self, name: str, args: List[Tuple]) -> Any:
        if name not in self.functions:
            raise NameError(f"Function '{name}' not defined")
        
        params, block = self.functions[name]
        
        if len(args) != len(params):
            raise ValueError(
                f"Function '{name}' expects {len(params)} args, "
                f"got {len(args)}"
            )
        
        # Save state
        old_vars = self.variables.copy()
        old_defined = self.defined_variables.copy()
        
        # Bind parameters
        for param, arg in zip(params, args):
            self.variables[param] = self.eval_expression(arg)
            self.defined_variables.add(param)
        
        # Execute function
        self.return_value = None
        for stmt in block:
            result = self.execute(stmt)
            if result == 'return':
                break
        
        result = self.return_value
        
        # Restore state
        self.variables = old_vars
        self.defined_variables = old_defined
        
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
            # Check if variable is defined
            if var_name not in self.defined_variables:
                raise NameError(
                    f"Variable '{var_name}' used before assignment"
                )
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

    def get_type(self, value: Any) -> str:
        """Get Kronos type name from Python value"""
        if isinstance(value, bool):
            return 'bool'
        elif isinstance(value, (int, float)):
            return 'number'
        elif isinstance(value, str):
            return 'string'
        else:
            return 'unknown'