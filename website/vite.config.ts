import { tanstackStart } from "@tanstack/react-start/plugin/vite";
import viteReact from "@vitejs/plugin-react";
import fumadocsMdx from "fumadocs-mdx/vite";
import { defineConfig } from "vite";
import tsconfigPaths from "vite-tsconfig-paths";
import * as sourceConfig from "./source.config";

export default defineConfig(async () => ({
  server: {
    port: 3000,
  },
  plugins: [
    await fumadocsMdx(sourceConfig, { index: false }),
    tanstackStart(),
    tsconfigPaths(),
    viteReact(),
  ],
}));
