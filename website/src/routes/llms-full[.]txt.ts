import { getLLMText, source } from "~/lib/source";
import { createFileRoute } from "@tanstack/react-router";

async function GET() {
  const scan = source.getPages().map(getLLMText);
  const scanned = await Promise.all(scan);

  return new Response(scanned.join("\n\n"), {
    headers: {
      "content-type": "text/plain; charset=utf-8",
    },
  });
}

export const Route = createFileRoute("/llms-full.txt")({
  server: {
    handlers: {
      GET,
    },
  },
});
