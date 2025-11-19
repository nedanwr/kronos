# Kronos Language Support for VSCode

Provides language support for Kronos (`.kr`) files including:

- Syntax highlighting
- Real-time error detection
- Autocomplete
- Hover information
- Code formatting

## Features

- **Syntax Highlighting**: Full syntax highlighting for Kronos keywords, operators, and literals
- **Language Server**: Real-time diagnostics and code intelligence
- **Auto-indentation**: Smart indentation for blocks
- **Bracket Matching**: Automatic bracket and quote closing

## Requirements

- The Kronos LSP server (`kronos-lsp`) must be built in your workspace
- Run `make lsp` in the Kronos project root

## Extension Settings

This extension contributes the following settings:

* `kronos.lsp.enabled`: Enable/disable the Kronos Language Server
* `kronos.lsp.trace.server`: Set trace level for LSP communication (off/messages/verbose)

## Commands

* `Kronos: Restart Language Server` - Restart the LSP server

## Known Issues

- LSP server is currently in early development
- Error messages don't include line/column information yet

## Release Notes

### 0.1.0

Initial release with:
- Syntax highlighting
- Basic LSP integration
- Error detection
- Keyword autocomplete

