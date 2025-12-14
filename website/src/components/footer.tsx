"use client";

export function Footer() {
  const footerLinks = {
    Resources: ["Documentation", "Learn", "Playground", "Blog"],
    Project: ["About", "Roadmap", "GitHub"],
  };

  return (
    <footer className="border-t border-white/10 bg-[#0a0a0a]/50 backdrop-blur-sm">
      <div className="mx-auto max-w-7xl px-4 py-16 sm:px-6 lg:px-8">
        <div className="grid gap-12 lg:grid-cols-2">
          <div>
            <span
              className="text-xl font-semibold text-[#F59E0B]"
              style={{
                textShadow:
                  "0 0 20px rgba(245, 158, 11, 0.4), 0 0 40px rgba(245, 158, 11, 0.2)",
              }}
            >
              Kronos
            </span>
            <p className="mb-6 mt-4 max-w-md text-[#9CA3AF]">
              A modern programming language with clean, English-like syntax.
              Simple to learn, powerful enough for real projects.
            </p>
          </div>

          {/* Links */}
          <div className="grid grid-cols-2 gap-8">
            {Object.entries(footerLinks).map(([category, links]) => (
              <div key={category}>
                <h3 className="mb-4 text-sm font-semibold text-white">
                  {category}
                </h3>
                <ul className="space-y-2">
                  {links.map((link) => (
                    <li key={link}>
                      <a
                        href="#"
                        className="text-sm text-[#4B5563] transition-colors hover:text-[#F59E0B]"
                      >
                        {link}
                      </a>
                    </li>
                  ))}
                </ul>
              </div>
            ))}
          </div>
        </div>

        <div className="mt-12 flex flex-col items-center gap-4 border-t border-white/10 pt-8 sm:flex-row">
          <p className="text-sm text-[#4B5563]">
            © 2025 Kronos Project. Open source under MIT License.
          </p>
        </div>
      </div>
    </footer>
  );
}
