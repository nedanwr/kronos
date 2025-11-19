#!/bin/bash
# Manually test the Kronos LSP server

# Exit on error
set -e

echo "🧪 Testing Kronos LSP Server Manually"
echo ""

if [ ! -f "./kronos-lsp" ]; then
    echo "Building LSP server first..."
    make lsp
    echo "✓ Build successful"
    echo ""
fi

echo "Starting LSP server (will read from stdin)..."
echo "Send JSON-RPC requests, or press Ctrl+C to exit."
echo ""
echo "Example request to paste:"
echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
echo ""
echo "---"
echo ""

./kronos-lsp

