import { DocsLayout } from "fumadocs-ui/layouts/docs";

import { source } from "~/lib/source";

export default function Layout({ children }: LayoutProps<"/docs">) {
  return (
    <DocsLayout
      nav={{
        title: (
          <>
            <span className="text-[#F59E0B]">⏱</span>
            <span>Kronos</span>
          </>
        ),
        url: "/",
        transparentMode: "always",
      }}
      tree={source.pageTree}
      themeSwitch={{
        enabled: false,
      }}
      sidebar={{
        banner: (
          <div className="mb-4 rounded-lg border border-[#F59E0B]/20 bg-[#F59E0B]/5 p-3">
            <p className="text-xs text-[#9CA3AF]">
              <span className="font-medium text-[#F59E0B]">v0.4.5</span> —
              Documentation
            </p>
          </div>
        ),
      }}
    >
      {children}
    </DocsLayout>
  );
}
