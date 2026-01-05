import { readFileSync } from "fs";
import { join } from "path";
import {
  defineConfig,
  defineDocs,
  frontmatterSchema,
  metaSchema,
} from "fumadocs-mdx/config";

// Load Kronos grammar synchronously at build time
const kronosGrammar = JSON.parse(
  readFileSync(join(process.cwd(), "lib/kronos.tmLanguage.json"), "utf8")
);

// You can customise Zod schemas for frontmatter and `meta.json` here
// see https://fumadocs.dev/docs/mdx/collections
export const docs = defineDocs({
  dir: "content/docs",
  docs: {
    schema: frontmatterSchema,
    postprocess: {
      includeProcessedMarkdown: true,
    },
  },
  meta: {
    schema: metaSchema,
  },
});

export default defineConfig({
  mdxOptions: {
    rehypeCodeOptions: {
      langs: [kronosGrammar],
      theme: "github-dark",
    },
  },
});
