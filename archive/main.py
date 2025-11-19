import sys
from src import Tokenizer, Parser, Interpreter

def run_file(filepath: str):
    with open(filepath, 'r') as f:
        code = f.read()
    
    tokenizer = Tokenizer(code)
    parser = Parser(tokenizer.tokens)
    ast = parser.parse()
    interpreter = Interpreter()
    interpreter.run(ast)

def repl():
    print("Kronos REPL - Type 'exit' to quit")
    interpreter = Interpreter()
    
    while True:
        try:
            line = input(">>> ")
            if line.strip() == 'exit':
                break
            
            tokenizer = Tokenizer(line)
            parser = Parser(tokenizer.tokens)
            ast = parser.parse()
            interpreter.run(ast)
        except Exception as e:
            print(f"Error: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        run_file(sys.argv[1])
    else:
        repl()