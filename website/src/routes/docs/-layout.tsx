import { DocsLayout } from "fumadocs-ui/layouts/docs";
import type { Root } from "fumadocs-core/page-tree";
import type { ReactNode } from "react";

export function DocsShell({
  children,
  tree,
}: {
  children: ReactNode;
  tree: Root;
}) {
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
      tree={tree}
      themeSwitch={{
        enabled: false,
      }}
      sidebar={{
        tabs: false,
        banner: (
          <div className="mb-4 rounded-lg border border-[#F59E0B]/20 bg-[#F59E0B]/5 p-3">
            <p className="text-xs text-[#9CA3AF]">
              <span className="font-medium text-[#F59E0B]">v0.5.x</span> —
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
