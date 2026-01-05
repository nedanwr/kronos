"use client";

import { useState, useEffect } from "react";
import { Github, Search, Menu, X } from "lucide-react";
import { Button } from "~/components/ui/button";

export function Navigation() {
  const [scrolled, setScrolled] = useState(false);
  const [mobileMenuOpen, setMobileMenuOpen] = useState(false);

  useEffect(() => {
    const handleScroll = () => {
      setScrolled(window.scrollY > 20);
    };
    window.addEventListener("scroll", handleScroll);
    return () => window.removeEventListener("scroll", handleScroll);
  }, []);

  const navLinks = [
    { name: "Learn", href: "#learn" },
    { name: "Docs", href: "/docs" },
    { name: "Playground", href: "/playground" },
  ];

  return (
    <header
      className={`fixed left-0 right-0 top-0 z-50 transition-all duration-300 ${
        scrolled
          ? "border-b border-white/10 bg-[#050505]/80 backdrop-blur-xl"
          : "bg-transparent backdrop-blur-sm"
      }`}
    >
      <div className="mx-auto flex h-16 max-w-7xl items-center justify-between px-4 sm:px-6 lg:px-8">
        <a href="/" className="flex items-center">
          <span
            className="text-xl font-semibold text-[#F59E0B]"
            style={{
              textShadow:
                "0 0 20px rgba(245, 158, 11, 0.4), 0 0 40px rgba(245, 158, 11, 0.2)",
            }}
          >
            Kronos
          </span>
        </a>

        {/* Desktop Navigation */}
        <nav className="hidden items-center gap-8 md:flex">
          {navLinks.map((link) => (
            <a
              key={link.name}
              href={link.href}
              className="text-sm text-[#9CA3AF] transition-colors hover:text-white"
            >
              {link.name}
            </a>
          ))}
        </nav>

        {/* Right side actions */}
        <div className="hidden items-center gap-4 md:flex">
          <Button
            variant="ghost"
            size="sm"
            className="gap-2 text-[#9CA3AF] hover:bg-white/5 hover:text-white"
          >
            <Search className="h-4 w-4" />
            <span className="text-xs text-[#4B5563]">⌘K</span>
          </Button>
          <a
            href="https://github.com/nedanwr/kronos"
            target="_blank"
            rel="noopener noreferrer"
            className="text-[#9CA3AF] transition-colors hover:text-white"
          >
            <Github className="h-5 w-5" />
          </a>
        </div>

        {/* Mobile menu button */}
        <button
          className="text-[#9CA3AF] md:hidden"
          onClick={() => setMobileMenuOpen(!mobileMenuOpen)}
        >
          {mobileMenuOpen ? (
            <X className="h-6 w-6" />
          ) : (
            <Menu className="h-6 w-6" />
          )}
        </button>
      </div>

      {/* Mobile menu */}
      {mobileMenuOpen && (
        <div className="border-t border-white/10 bg-[#050505]/95 backdrop-blur-xl md:hidden">
          <nav className="flex flex-col gap-4 p-4">
            {navLinks.map((link) => (
              <a
                key={link.name}
                href={link.href}
                className="text-[#9CA3AF] transition-colors hover:text-white"
                onClick={() => setMobileMenuOpen(false)}
              >
                {link.name}
              </a>
            ))}
            <a
              href="https://github.com/nedanwr/kronos"
              target="_blank"
              rel="noopener noreferrer"
              className="flex items-center gap-2 text-[#9CA3AF] transition-colors hover:text-white"
            >
              <Github className="h-5 w-5" />
              <span>GitHub</span>
            </a>
          </nav>
        </div>
      )}
    </header>
  );
}
