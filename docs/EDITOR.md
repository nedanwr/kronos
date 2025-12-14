# Kronos Editor Support

Full IDE support with syntax highlighting, error checking, and autocomplete.

## Quick Install

```bash
./scripts/install_extension.sh
```

Restart your editor (VSCode/Cursor/Windsurf). Done! ✅

## Features

✅ Syntax highlighting
✅ Real-time error detection
✅ Keyword autocomplete
✅ Auto-indentation & bracket matching
✅ Code folding

## Troubleshooting

**No syntax highlighting?**

- Check file extension is `.kr`
- Check bottom-right shows "Kronos"
- Try: Cmd+Shift+P → "Change Language Mode" → "Kronos"

**LSP not working?**

```bash
make clean && make lsp
./scripts/install_extension.sh
# Restart editor
```

**View LSP logs:**
View → Output → "Kronos Language Server"

## Other Editors

### Vim/Neovim

Create `~/.vim/syntax/kronos.vim`:

```vim
syn keyword kronosKeyword set let to as if for while function return print
syn keyword kronosBoolean true false
syn keyword kronosConstant Pi null
syn region kronosString start='"' end='"'
syn match kronosNumber '\<\d\+\(\.\d\+\)\?\>'
syn match kronosComment '#.*$'

hi def link kronosKeyword Keyword
hi def link kronosBoolean Boolean
hi def link kronosConstant Constant
hi def link kronosString String
hi def link kronosNumber Number
hi def link kronosComment Comment
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
    - match: '\b(set|let|if|for|while|function|return|print)\b'
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

## LSP Architecture

```
Editor → Extension → kronos-lsp → Parser → Diagnostics
```

The LSP server (`src/lsp/lsp_server.c`) provides real-time syntax checking and autocomplete by parsing your code as you type.
