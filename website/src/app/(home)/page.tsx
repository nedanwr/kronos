import {
  ArrowRight,
  BookOpen,
  Package,
  Rocket,
  ShieldCheck,
  Terminal,
} from "lucide-react";
import Link from "next/link";

import { Navigation } from "~/components/navigation";
import { Button } from "~/components/ui/button";
import { CodeShowcase } from "~/components/code-showcase";
import { Footer } from "~/components/footer";

const features = [
  {
    icon: BookOpen,
    title: "Readable by Design",
    description:
      "English-like syntax that's easy to learn and read. No cryptic symbols.",
  },
  {
    icon: Package,
    title: "Batteries Included",
    description: "File I/O, regex, math modules, and more built right in.",
  },
  {
    icon: ShieldCheck,
    title: "Modern Error Handling",
    description:
      "Try/catch/finally blocks for clean, predictable error management.",
  },
  {
    icon: Rocket,
    title: "Fast Iteration",
    description: "Simple toolchain. Just `kronos file.kr` and go.",
  },
];

export default function HomePage() {
  return (
    <div className="relative min-h-screen overflow-hidden bg-[#050505]">
      <div className="pointer-events-none fixed inset-0 overflow-hidden">
        <div className="absolute -right-1/4 -top-1/4 h-[800px] w-[800px] rounded-full bg-linear-to-bl from-[#2E1065]/60 via-[#6D28D9]/20 to-transparent blur-3xl opacity-30 sm:opacity-50 lg:opacity-100" />
        <div className="absolute -bottom-1/4 -left-1/4 h-[700px] w-[700px] rounded-full bg-linear-to-tr from-[#1E1B4B]/50 via-[#2E1065]/30 to-transparent blur-3xl opacity-30 sm:opacity-50 lg:opacity-100" />
        <div className="absolute inset-0 bg-[url('data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIzMDAiIGhlaWdodD0iMzAwIj48ZmlsdGVyIGlkPSJhIiB4PSIwIiB5PSIwIj48ZmVUdXJidWxlbmNlIGJhc2VGcmVxdWVuY3k9Ii43NSIgc3RpdGNoVGlsZXM9InN0aXRjaCIgdHlwZT0iZnJhY3RhbE5vaXNlIi8+PGZlQ29sb3JNYXRyaXggdHlwZT0ic2F0dXJhdGUiIHZhbHVlcz0iMCIvPjwvZmlsdGVyPjxyZWN0IHdpZHRoPSIxMDAlIiBoZWlnaHQ9IjEwMCUiIGZpbHRlcj0idXJsKCNhKSIgb3BhY2l0eT0iMC4wNSIvPjwvc3ZnPg==')] opacity-50" />
      </div>

      <div className="relative z-10">
        <Navigation />
        <main>
          {/* Hero Section */}
          <section className="relative px-4 pb-24 pt-32 sm:px-6 lg:px-8">
            <div className="mx-auto max-w-7xl">
              <div className="grid items-center gap-12 lg:grid-cols-2">
                <div className="text-center lg:text-left">
                  <div className="mb-6 inline-flex items-center gap-2 rounded-full border border-white/10 bg-white/5 px-4 py-2 backdrop-blur-sm">
                    <span className="h-2 w-2 rounded-full bg-[#F59E0B]" />
                    <span className="text-sm text-[#E5E5E5]">
                      v0.4.0 — Now in public beta
                    </span>
                  </div>

                  <h1 className="mb-6 text-4xl font-bold leading-tight tracking-tight sm:text-5xl lg:text-6xl">
                    <span className="text-white">Build faster with </span>
                    <span className="text-white">Kronos</span>
                  </h1>

                  <p className="mx-auto mb-8 max-w-xl text-lg leading-relaxed text-[#9CA3AF] lg:mx-0">
                    A modern programming language with clean, English-like
                    syntax. Simple to learn, powerful enough for real projects.
                  </p>

                  <div className="flex flex-col justify-center gap-4 sm:flex-row lg:justify-start">
                    <Link href="/docs/getting-started">
                      <Button
                        size="lg"
                        className="group relative overflow-hidden border border-[#F59E0B]/50 bg-[rgba(245,158,11,0.15)] text-[#F59E0B] font-semibold backdrop-blur-sm transition-all hover:border-[#F59E0B] hover:bg-[rgba(245,158,11,0.25)] hover:text-[#FBBF24] hover:shadow-lg hover:shadow-[#F59E0B]/30"
                      >
                        <span className="relative z-10 flex items-center gap-2">
                          Get Started
                          <ArrowRight className="h-4 w-4 transition-transform group-hover:translate-x-1" />
                        </span>
                      </Button>
                    </Link>
                    <Link href="/docs">
                      <Button
                        size="lg"
                        variant="outline"
                        className="border-white/20 bg-white/5 text-white backdrop-blur-sm hover:bg-white/10 hover:text-white"
                      >
                        Read the Docs
                      </Button>
                    </Link>
                  </div>
                </div>

                <div className="relative">
                  <div className="absolute -inset-4 rounded-3xl bg-[rgba(245,158,11,0.1)] blur-2xl" />

                  <div className="relative overflow-hidden rounded-2xl border border-[rgba(245,158,11,0.15)] bg-[#0a0a0a]/80 backdrop-blur-xl">
                    <div className="flex items-center gap-2 border-b border-white/10 bg-white/5 px-4 py-3">
                      <div className="flex gap-2">
                        <div className="h-3 w-3 rounded-full bg-red-500/80" />
                        <div className="h-3 w-3 rounded-full bg-yellow-500/80" />
                        <div className="h-3 w-3 rounded-full bg-green-500/80" />
                      </div>
                      <div className="flex items-center gap-2 text-sm text-[#4B5563]">
                        <Terminal className="h-4 w-4" />
                        main.kr
                      </div>
                    </div>

                    <pre className="overflow-x-auto p-6">
                      <code className="font-mono text-sm leading-relaxed">
                        <span className="text-[#8B5CF6]">set</span>{" "}
                        <span className="text-white">name</span>{" "}
                        <span className="text-[#8B5CF6]">to</span>{" "}
                        <span className="text-green-400">"Kronos"</span>
                        {"\n"}
                        <span className="text-[#8B5CF6]">set</span>{" "}
                        <span className="text-white">numbers</span>{" "}
                        <span className="text-[#8B5CF6]">to</span>{" "}
                        <span className="text-[#F59E0B]">list</span>{" "}
                        <span className="text-[#A78BFA]">1</span>
                        <span className="text-[#4B5563]">,</span>{" "}
                        <span className="text-[#A78BFA]">2</span>
                        <span className="text-[#4B5563]">,</span>{" "}
                        <span className="text-[#A78BFA]">3</span>
                        <span className="text-[#4B5563]">,</span>{" "}
                        <span className="text-[#A78BFA]">4</span>
                        <span className="text-[#4B5563]">,</span>{" "}
                        <span className="text-[#A78BFA]">5</span>
                        {"\n\n"}
                        <span className="text-[#8B5CF6]">function</span>{" "}
                        <span className="text-[#F59E0B]">greet</span>{" "}
                        <span className="text-[#8B5CF6]">with</span>{" "}
                        <span className="text-white">person</span>
                        <span className="text-[#4B5563]">:</span>
                        {"\n"}
                        {"    "}
                        <span className="text-[#8B5CF6]">return</span>{" "}
                        <span className="text-green-400">f"Hello, {"{"}</span>
                        <span className="text-white">person</span>
                        <span className="text-green-400">{"}"}!"</span>
                        {"\n\n"}
                        <span className="text-[#8B5CF6]">for</span>{" "}
                        <span className="text-white">n</span>{" "}
                        <span className="text-[#8B5CF6]">in</span>{" "}
                        <span className="text-white">numbers</span>
                        <span className="text-[#4B5563]">:</span>
                        {"\n"}
                        {"    "}
                        <span className="text-[#F59E0B]">print</span>{" "}
                        <span className="text-[#8B5CF6]">call</span>{" "}
                        <span className="text-[#F59E0B]">greet</span>{" "}
                        <span className="text-[#8B5CF6]">with</span>{" "}
                        <span className="text-white">name</span>
                      </code>
                    </pre>
                  </div>
                </div>
              </div>
            </div>
          </section>
          {/* Why Kronos Section */}
          <section id="learn" className="relative px-4 py-24 sm:px-6 lg:px-8">
            <div className="mx-auto max-w-7xl">
              <div className="mb-16 text-center">
                <h2 className="mb-4 text-3xl font-bold text-white sm:text-4xl">
                  Why <span className="text-white">Kronos</span>?
                </h2>
                <p className="mx-auto max-w-2xl text-[#9CA3AF]">
                  Designed for developers who want clean, readable code without
                  sacrificing power.
                </p>
              </div>

              <div className="grid gap-6 sm:grid-cols-2 lg:grid-cols-4">
                {features.map((feature) => (
                  <div
                    key={feature.title}
                    className="group relative overflow-hidden rounded-2xl border border-white/10 bg-[rgba(255,255,255,0.05)] p-6 backdrop-blur-md transition-all duration-300 hover:-translate-y-1 hover:border-white/20 hover:bg-[rgba(255,255,255,0.08)]"
                  >
                    <div className="pointer-events-none absolute -inset-px rounded-2xl bg-linear-to-r from-[#8B5CF6]/0 via-[#A78BFA]/0 to-[#8B5CF6]/0 opacity-0 transition-opacity duration-300 group-hover:opacity-100 group-hover:from-[#8B5CF6]/10 group-hover:via-[#A78BFA]/10 group-hover:to-[#8B5CF6]/10" />

                    <div className="relative">
                      <div className="mb-4 flex h-12 w-12 items-center justify-center rounded-xl bg-[#F59E0B]/10 text-[#F59E0B]">
                        <feature.icon className="h-6 w-6" />
                      </div>
                      <h3 className="mb-2 text-lg font-semibold text-white">
                        {feature.title}
                      </h3>
                      <p className="text-sm leading-relaxed text-[#9CA3AF]">
                        {feature.description}
                      </p>
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </section>
          <CodeShowcase />
        </main>
        <Footer />
      </div>
    </div>
  );
}
