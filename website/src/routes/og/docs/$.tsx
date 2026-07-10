import { getPageImage, source } from "~/lib/source";
import { createFileRoute } from "@tanstack/react-router";

export const Route = createFileRoute("/og/docs/$")({
  server: {
    handlers: {
      GET: ({ params }) => {
        const slug = (params._splat ?? "").split("/").filter(Boolean);
        const page = source.getPage(slug.slice(0, -1));

        if (!page) {
          return new Response("Not found", { status: 404 });
        }

        return new Response(
          createOgSvg(page.data.title, page.data.description ?? ""),
          {
            headers: {
              "content-type": "image/svg+xml; charset=utf-8",
              "cache-control": "public, max-age=31536000, immutable",
            },
          }
        );
      },
    },
  },
});

export function getStaticOgPaths() {
  return source.getPages().map((page) => getPageImage(page).segments);
}

function createOgSvg(title: string, description: string) {
  return `<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="630" viewBox="0 0 1200 630">
  <defs>
    <linearGradient id="bg" x1="0" x2="1" y1="0" y2="1">
      <stop offset="0%" stop-color="#050505"/>
      <stop offset="55%" stop-color="#111827"/>
      <stop offset="100%" stop-color="#1E1B4B"/>
    </linearGradient>
  </defs>
  <rect width="1200" height="630" fill="url(#bg)"/>
  <circle cx="1030" cy="70" r="280" fill="#F59E0B" opacity="0.14"/>
  <circle cx="120" cy="560" r="240" fill="#6D28D9" opacity="0.18"/>
  <text x="96" y="132" fill="#F59E0B" font-family="Inter, Arial, sans-serif" font-size="34" font-weight="700">Kronos Docs</text>
  <text x="96" y="292" fill="#FFFFFF" font-family="Inter, Arial, sans-serif" font-size="72" font-weight="800">${escapeSvg(title)}</text>
  <text x="96" y="382" fill="#D1D5DB" font-family="Inter, Arial, sans-serif" font-size="32">${escapeSvg(description)}</text>
  <rect x="96" y="500" width="220" height="4" rx="2" fill="#F59E0B"/>
</svg>`;
}

function escapeSvg(value: string) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}
