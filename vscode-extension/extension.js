const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
  console.log('Kronos extension activating...');

  const config = vscode.workspace.getConfiguration('kronos');
  
  if (!config.get('lsp.enabled')) {
    console.log('Kronos LSP is disabled in settings');
    return;
  }

  // Find the workspace folder containing the LSP binary
  const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
  if (!workspaceFolder) {
    console.error('No workspace folder found');
    return;
  }

  // Path to the LSP server binary (add .exe on Windows)
  const binaryName = process.platform === 'win32' ? 'kronos-lsp.exe' : 'kronos-lsp';
  const serverPath = path.join(workspaceFolder.uri.fsPath, binaryName);

  if (!fs.existsSync(serverPath)) {
    const message = `Kronos LSP binary not found at ${serverPath}. Run "make lsp" first.`;
    console.error(message);
    vscode.window.showErrorMessage(message);
    return;
  }

  const serverOptions = {
    command: serverPath,
    args: [],
    transport: TransportKind.stdio
  };

  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'kronos' },
      { scheme: 'untitled', language: 'kronos' }
    ],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.kr')
    },
    outputChannelName: 'Kronos Language Server'
  };

  try {
    client = new LanguageClient(
      'kronosLanguageServer',
      'Kronos Language Server',
      serverOptions,
      clientOptions
    );

    // Start the client (which starts the server)
    client.start();
    
    console.log('Kronos LSP client started');

    context.subscriptions.push(
      vscode.commands.registerCommand('kronos.restartServer', async () => {
        if (client) {
          await client.stop();
          await client.start();
          vscode.window.showInformationMessage('Kronos Language Server restarted');
        }
      })
    );

  } catch (error) {
    console.error('Failed to start Kronos LSP:', error);
    vscode.window.showErrorMessage(`Failed to start Kronos Language Server: ${error.message}`);
  }
}

function deactivate() {
  if (client) {
    return client.stop();
  }
}

module.exports = {
  activate,
  deactivate
};

