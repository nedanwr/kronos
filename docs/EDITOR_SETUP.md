# Kronos Editor Setup

Get full IDE support for Kronos with syntax highlighting and real-time error checking.

## Quick Install (VSCode/Cursor/Windsurf)

```bash
./install_extension.sh
```

Then restart your editor. **Done!** ✅

## What You Get

✅ **Syntax Highlighting** - Keywords, strings, numbers color-coded  
✅ **Real-time Error Detection** - Red squiggly lines under errors  
✅ **Autocomplete** - Keyword suggestions as you type  
✅ **Bracket Matching** - Auto-close quotes and brackets  
✅ **Auto-indentation** - Smart indentation after `:` and `with`  
✅ **Code Folding** - Collapse blocks

🔄 **Coming Soon:**

- Hover information
- Go to definition
- Rename refactoring
- Function signature help

## Troubleshooting

### Extension Not Loading?

1. Check: **View → Output → "Kronos Language Server"**
2. Look for error messages

### No Syntax Highlighting?

1. Check file extension is `.kr`
2. Check bottom-right of editor - should say "Kronos"
3. Try: **Cmd+Shift+P** → "Change Language Mode" → "Kronos"

### LSP Not Starting?

```bash
# Rebuild and reinstall
make clean && make lsp
./install_extension.sh

# Restart editor completely
```

## Other Editors

### Vim/Neovim

Create `~/.vim/syntax/kronos.vim`:

```vim
" Kronos syntax file
if exists("b:current_syntax")
  finish
endif

" Keywords
syn keyword kronosKeyword set let to as if for while in range function with call return print
syn keyword kronosBoolean true false
syn keyword kronosNull null
syn keyword kronosConstant Pi

" Operators
syn keyword kronosOperator plus minus times divided by is equal not greater less than and or

" Strings
syn region kronosString start='"' end='"'

" Numbers
syn match kronosNumber '\<\d\+\(\.\d\+\)\?\>'

" Comments
syn match kronosComment '#.*$'

" Highlighting
hi def link kronosKeyword Keyword
hi def link kronosBoolean Boolean
hi def link kronosNull Constant
hi def link kronosConstant Constant
hi def link kronosOperator Operator
hi def link kronosString String
hi def link kronosNumber Number
hi def link kronosComment Comment

let b:current_syntax = "kronos"
```

Add to `~/.vim/ftdetect/kronos.vim`:

```vim
au BufRead,BufNewFile *.kr set filetype=kronos
```

### Sublime Text

Create `Kronos.sublime-syntax`:

```yaml
%YAML 1.2
---
name: Kronos
file_extensions: [kr]
scope: source.kronos

contexts:
  main:
    - match: "#.*$"
      scope: comment.line.kronos

    - match: '\b(set|let|if|for|while|function|call|return|print)\b'
      scope: keyword.control.kronos

    - match: '\b(true|false|null|Pi)\b'
      scope: constant.language.kronos

    - match: '"'
      push: string

    - match: '\b\d+(\.\d+)?\b'
      scope: constant.numeric.kronos

  string:
    - meta_scope: string.quoted.double.kronos
    - match: '"'
      pop: true
```

### Emacs

Add to `~/.emacs` or `~/.emacs.d/init.el`:

```elisp
(define-derived-mode kronos-mode prog-mode "Kronos"
  "Major mode for editing Kronos files."
  (setq-local comment-start "#")
  (setq-local comment-end "")

  (setq font-lock-defaults
        '((("\\(set\\|let\\|if\\|for\\|while\\|function\\|call\\|return\\|print\\)" . font-lock-keyword-face)
           ("\\(true\\|false\\|null\\|Pi\\)" . font-lock-constant-face)
           ("\"[^\"]*\"" . font-lock-string-face)
           ("#.*$" . font-lock-comment-face)))))

(add-to-list 'auto-mode-alist '("\\.kr\\'" . kronos-mode))
```

## LSP Architecture

```
Your Editor (VSCode/Cursor/Windsurf)
    ↓
Extension (vscode-extension/)
    ↓ (JSON-RPC via stdio)
LSP Server (kronos-lsp)
    ↓
Parser (tokenize → parse → check errors)
    ↓
Diagnostics sent back to editor
```

## Advanced: Debugging the LSP

### Check if LSP is Running

```bash
ps aux | grep kronos-lsp
```

### View Extension Logs

In VSCode: **View → Output** → Select **"Kronos Language Server"** from dropdown

### Test LSP Manually

```bash
./test_lsp_manually.sh
```

Or send raw JSON-RPC:

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | ./kronos-lsp
```

## Extension Development

The extension is in `vscode-extension/`:

```
vscode-extension/
├── package.json              # Extension manifest
├── extension.js              # LSP client code
├── syntaxes/                 # Syntax highlighting
│   └── kronos.tmLanguage.json
└── language-configuration.json
```

To modify:

1. Edit files in `vscode-extension/`
2. Run `./install_extension.sh` again
3. Restart your editor

## Contributing

Want to improve editor support?

1. Add more completion items to LSP
2. Implement hover information
3. Add snippets for common patterns
4. Create syntax highlighting for more editors
5. Implement semantic tokens

See `src/lsp/lsp_server.c` for the LSP implementation.
