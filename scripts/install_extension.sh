#!/bin/bash
# Install Kronos VSCode extension locally

set -e

echo "📦 Installing Kronos VSCode Extension..."
echo ""

# Determine script directory and project root
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

if [ -z "$SCRIPT_DIR" ] || [ -z "$PROJECT_ROOT" ]; then
    echo "❌ Error: Failed to resolve script or project root directory."
    exit 1
fi

cd "$PROJECT_ROOT"

# Always rebuild the LSP server to ensure it's up to date
echo "🔨 Building LSP server..."
make lsp
echo "✅ LSP server built"
echo ""

# Install npm dependencies
echo "1️⃣  Installing extension dependencies..."
if [ ! -d "$PROJECT_ROOT/vscode-extension" ]; then
    echo "❌ Error: vscode-extension directory not found at $PROJECT_ROOT/vscode-extension"
    exit 1
fi

cd "$PROJECT_ROOT/vscode-extension"

if ! command -v npm &> /dev/null; then
    echo "❌ Error: npm not found. Please install Node.js first."
    exit 1
fi

npm install --silent
echo "✅ Dependencies installed"
echo ""

cd "$PROJECT_ROOT"

# Get extension directory for each editor
VSCODE_EXT="$HOME/.vscode/extensions"
CURSOR_EXT="$HOME/.cursor/extensions"
WINDSURF_EXT="$HOME/.windsurf/extensions"

INSTALLED=false

# Function to install extension
install_to_editor() {
    local EXT_DIR=$1
    local EDITOR_NAME=$2

    if [ -d "$EXT_DIR" ]; then
        echo "2️⃣  Installing to $EDITOR_NAME..."

        TARGET="$EXT_DIR/kronos-lsp-0.1.0"

        # Remove old version if exists
        rm -rf "$TARGET"

        # Copy extension
        cp -r "$PROJECT_ROOT/vscode-extension" "$TARGET"

        echo "✅ Installed to $TARGET"
        INSTALLED=true
    fi
}

# Try to install to each editor
install_to_editor "$VSCODE_EXT" "VSCode"
install_to_editor "$CURSOR_EXT" "Cursor"
install_to_editor "$WINDSURF_EXT" "Windsurf"

if [ "$INSTALLED" = false ]; then
    echo "⚠️  No supported editors found."
    echo ""
    echo "Supported editors:"
    echo "  - VSCode: $VSCODE_EXT"
    echo "  - Cursor: $CURSOR_EXT"
    echo "  - Windsurf: $WINDSURF_EXT"
    echo ""
    echo "Please install one of these editors first."
    exit 1
fi

echo ""
echo "3️⃣  Final steps:"
echo ""
echo "  1. Restart your editor (VSCode/Cursor/Windsurf)"
echo "     - Close all windows"
echo "     - Reopen the Kronos project"
echo ""
echo "  2. Open a .kr file (e.g., examples/hello.kr)"
echo ""
echo "  3. Check the Output panel:"
echo "     - View → Output"
echo "     - Select 'Kronos Language Server' from dropdown"
echo ""
echo "  4. You should now see:"
echo "     ✅ Syntax highlighting"
echo "     ✅ Real-time error checking"
echo "     ✅ Autocomplete (press space after keywords)"
echo ""
echo "🎉 Installation complete!"
