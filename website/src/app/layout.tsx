import { RootProvider } from "fumadocs-ui/provider/next";
import { Inter, JetBrains_Mono } from "next/font/google";
import "./global.css";

const inter = Inter({
  subsets: ["latin"],
  variable: "--font-inter",
});
const jetbrainsMono = JetBrains_Mono({
  subsets: ["latin"],
  variable: "--font-jetbrains-mono",
});

export default function Layout({ children }: LayoutProps<"/">) {
  return (
    <html lang="en" className="dark" suppressHydrationWarning>
      <body
        className={`${inter.className} ${jetbrainsMono.variable} font-sans antialiased`}
      >
        <RootProvider>{children}</RootProvider>
      </body>
    </html>
  );
}
