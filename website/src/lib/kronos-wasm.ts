/**
 * @file kronos-wasm.ts
 * @brief TypeScript wrapper for the Kronos WebAssembly module
 *
 * This module provides a clean interface for loading and executing
 * Kronos code in the browser via WebAssembly.
 */

// Types for the Emscripten module
interface KronosWasmModule {
  _kronos_wasm_init: () => number;
  _kronos_wasm_run: (sourcePtr: number) => number;
  _kronos_wasm_cleanup: () => void;
  _kronos_wasm_reset: () => void;
  _kronos_wasm_get_error: () => number;
  _kronos_wasm_get_warnings: () => number;
  _kronos_wasm_version: () => number;
  _malloc: (size: number) => number;
  _free: (ptr: number) => void;
  UTF8ToString: (ptr: number) => string;
  stringToUTF8: (str: string, ptr: number, maxBytes: number) => void;
  lengthBytesUTF8: (str: string) => number;
  ccall: <T>(
    name: string,
    returnType: string | null,
    argTypes: string[],
    args: unknown[]
  ) => T;
  cwrap: <T extends (...args: unknown[]) => unknown>(
    name: string,
    returnType: string | null,
    argTypes: string[]
  ) => T;
}

// Type for the module factory function
type CreateKronosModule = (
  options?: Partial<EmscriptenModuleOptions>
) => Promise<KronosWasmModule>;

// Options for customizing module initialization
interface EmscriptenModuleOptions {
  print: (text: string) => void;
  printErr: (text: string) => void;
  locateFile: (path: string, prefix: string) => string;
}

/**
 * Result of executing Kronos code
 */
export interface KronosExecutionResult {
  success: boolean;
  output: string;
  error?: string;
  warnings?: string;
}

/**
 * Kronos WASM Runtime
 *
 * Provides methods for executing Kronos code in the browser.
 */
// Extend window to include the Emscripten module factory
declare global {
  interface Window {
    createKronosModule?: CreateKronosModule;
  }
}

export class KronosRuntime {
  private module: KronosWasmModule | null = null;
  private initialized = false;
  private loading: Promise<void> | null = null;
  private outputBuffer: string[] = [];

  /**
   * Load the Emscripten-generated script and return the factory function
   */
  private async _loadScript(src: string): Promise<CreateKronosModule> {
    // Check if already loaded
    if (typeof window !== "undefined" && window.createKronosModule) {
      return window.createKronosModule;
    }

    return new Promise((resolve, reject) => {
      const script = document.createElement("script");
      script.src = src;
      script.async = true;

      script.onload = () => {
        // Emscripten with MODULARIZE assigns the factory to a global variable
        if (window.createKronosModule) {
          resolve(window.createKronosModule);
        } else {
          reject(new Error("createKronosModule not found after script load"));
        }
      };

      script.onerror = () => {
        reject(new Error(`Failed to load script: ${src}`));
      };

      document.head.appendChild(script);
    });
  }

  /**
   * Load and initialize the Kronos WASM module
   *
   * @returns Promise that resolves when the module is ready
   */
  async initialize(): Promise<void> {
    // Prevent multiple concurrent initializations
    if (this.loading) {
      return this.loading;
    }

    if (this.initialized) {
      return;
    }

    this.loading = this._doInitialize();
    await this.loading;
    this.loading = null;
  }

  private async _doInitialize(): Promise<void> {
    try {
      // Load the Emscripten-generated module via script tag
      // Emscripten's MODULARIZE option creates a factory function
      const createModule = await this._loadScript("/wasm/kronos.js");

      // Create the module instance with custom stdout/stderr capture
      this.module = await createModule({
        print: (text: string) => {
          this.outputBuffer.push(text);
        },
        printErr: (text: string) => {
          console.error("[Kronos]", text);
        },
        locateFile: (path: string) => {
          // WASM file is next to the JS file
          return `/wasm/${path}`;
        },
      });

      // Initialize the Kronos runtime
      const result = this.module._kronos_wasm_init();
      if (result !== 1) {
        throw new Error("Failed to initialize Kronos runtime");
      }

      this.initialized = true;
    } catch (error) {
      console.error("Failed to load Kronos WASM module:", error);
      throw error;
    }
  }

  /**
   * Check if the runtime is initialized
   */
  isInitialized(): boolean {
    return this.initialized;
  }

  /**
   * Execute Kronos source code
   *
   * @param source - Kronos source code to execute
   * @returns Execution result with output or error
   */
  async run(source: string): Promise<KronosExecutionResult> {
    if (!this.initialized || !this.module) {
      await this.initialize();
    }

    if (!this.module) {
      return {
        success: false,
        output: "",
        error: "Kronos runtime not initialized",
      };
    }

    // Clear output buffer
    this.outputBuffer = [];

    // Reset VM state before each run to clear variables from previous executions
    this.module._kronos_wasm_reset();

    try {
      // Allocate memory for the source string
      const sourceBytes = this.module.lengthBytesUTF8(source) + 1;
      const sourcePtr = this.module._malloc(sourceBytes);

      if (sourcePtr === 0) {
        return {
          success: false,
          output: "",
          error: "Failed to allocate memory for source code",
        };
      }

      // Copy source string to WASM memory
      this.module.stringToUTF8(source, sourcePtr, sourceBytes);

      // Execute the code
      const resultPtr = this.module._kronos_wasm_run(sourcePtr);

      // Free the source string memory
      this.module._free(sourcePtr);

      // Get the result string
      const resultStr = this.module.UTF8ToString(resultPtr);

      // Get any compiler warnings
      const warningsPtr = this.module._kronos_wasm_get_warnings();
      const warningsStr = this.module.UTF8ToString(warningsPtr);
      const warnings = warningsStr.trim() || undefined;

      // Check if it's an error
      if (resultStr.startsWith("Error:")) {
        return {
          success: false,
          output: this.outputBuffer.join("\n"),
          error: resultStr,
          warnings,
        };
      }

      // Combine captured output with any direct return
      const output = this.outputBuffer.join("\n");

      return {
        success: true,
        output: output || resultStr,
        warnings,
      };
    } catch (error) {
      return {
        success: false,
        output: this.outputBuffer.join("\n"),
        error:
          error instanceof Error ? error.message : "Unknown execution error",
      };
    }
  }

  /**
   * Reset the runtime state (clear variables and functions)
   */
  reset(): void {
    if (this.module && this.initialized) {
      this.module._kronos_wasm_reset();
    }
    this.outputBuffer = [];
  }

  /**
   * Get the Kronos version
   */
  getVersion(): string {
    if (!this.module || !this.initialized) {
      return "unknown";
    }
    const versionPtr = this.module._kronos_wasm_version();
    return this.module.UTF8ToString(versionPtr);
  }

  /**
   * Clean up the runtime (call when done)
   */
  cleanup(): void {
    if (this.module && this.initialized) {
      this.module._kronos_wasm_cleanup();
      this.initialized = false;
    }
    this.module = null;
    this.outputBuffer = [];
  }
}

// Singleton instance for convenience
let runtimeInstance: KronosRuntime | null = null;

/**
 * Get the singleton Kronos runtime instance
 */
export function getKronosRuntime(): KronosRuntime {
  if (!runtimeInstance) {
    runtimeInstance = new KronosRuntime();
  }
  return runtimeInstance;
}

/**
 * Execute Kronos code using the singleton runtime
 *
 * @param source - Kronos source code to execute
 * @returns Execution result
 */
export async function runKronos(
  source: string
): Promise<KronosExecutionResult> {
  const runtime = getKronosRuntime();
  return runtime.run(source);
}
