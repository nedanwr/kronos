import { createFileRoute, notFound } from "@tanstack/react-router";
import { createServerFn } from "@tanstack/react-start";
import {
  DocsBody,
  DocsDescription,
  DocsPage,
  DocsTitle,
} from "fumadocs-ui/layouts/docs/page";
import { getMDXComponents } from "~/mdx-components";
import { DocsShell } from "./-layout";
import { getPageImage, source } from "~/lib/source";
import browserCollections from "fumadocs-mdx:collections/browser";
import { useFumadocsLoader } from "fumadocs-core/source/client";

export const Route = createFileRoute("/docs/$")({
  head: () => ({
    meta: [
      { title: "Kronos Documentation" },
      {
        name: "description",
        content: "Language reference and API documentation for Kronos.",
      },
    ],
  }),
  loader: async ({ params }) => {
    const data = (await loadDocsPage({
      data: params._splat?.split("/").filter(Boolean) ?? [],
    })) as DocsLoaderData;

    await clientLoader.preload(data.path);
    return data;
  },
  component: DocsRoute,
});

const loadDocsPage = createServerFn({ method: "GET" })
  .validator((slugs: string[]) => slugs)
  .handler(async ({ data: slugs }) => {
    const page = source.getPage(slugs);

    if (!page) {
      throw notFound();
    }

    return {
      path: page.path,
      title: page.data.title,
      description: page.data.description,
      toc: page.data.toc.map((item) => ({
        ...item,
        title: toPlainText(item.title),
      })),
      full: page.data.full,
      image: getPageImage(page).url,
      tree: await source.serializePageTree(source.pageTree),
    };
  });

function toPlainText(value: unknown): string {
  if (value == null || typeof value === "boolean") {
    return "";
  }

  if (typeof value === "string" || typeof value === "number") {
    return String(value);
  }

  if (Array.isArray(value)) {
    return value.map(toPlainText).join("");
  }

  if (typeof value === "object") {
    const record = value as Record<string, unknown>;

    if (typeof record.value === "string" || typeof record.value === "number") {
      return String(record.value);
    }

    if (record.props && typeof record.props === "object") {
      return toPlainText((record.props as Record<string, unknown>).children);
    }

    if (record.children) {
      return toPlainText(record.children);
    }
  }

  return "";
}

const clientLoader = browserCollections.docs.createClientLoader({
  component({ default: MDX }) {
    return <MDX components={getMDXComponents()} />;
  },
});

function DocsRoute() {
  const data = Route.useLoaderData() as DocsLoaderData;
  const { pageTree } = useFumadocsLoader({ pageTree: data.tree });

  return (
    <DocsShell tree={pageTree}>
      <DocsPage
        toc={data.toc}
        full={data.full}
        tableOfContent={{
          style: "clerk",
        }}
      >
        <DocsTitle>{data.title}</DocsTitle>
        <DocsDescription>{data.description}</DocsDescription>
        <DocsBody>{clientLoader.useContent(data.path, {})}</DocsBody>
      </DocsPage>
    </DocsShell>
  );
}

type DocsLoaderData = {
  path: string;
  title: string;
  description?: string;
  toc: Array<{
    title: string;
    url: string;
    depth: number;
  }>;
  full?: boolean;
  image: string;
  tree: object;
};
