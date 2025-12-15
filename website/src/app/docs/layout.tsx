import { DocsLayout } from "fumadocs-ui/layouts/docs";

import { source } from "~/lib/source";

export default function Layout({ children }: LayoutProps<"/docs">) {
  return (
    <DocsLayout
      nav={{
        title: "Kronos",
      }}
      tree={source.pageTree}
      themeSwitch={{
        enabled: false,
      }}
    >
      {children}
    </DocsLayout>
  );
}
