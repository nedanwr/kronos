# Credits

This document recognizes people who have contributed to the Kronos
programming language project.

## Core Contributors

Core contributors are the primary maintainers and developers of Kronos.

- **Naveed Ali Anwar** ([@nedanwr](https://github.com/nedanwr)) - Project
  creator and lead maintainer

## Contributors

Contributors are individuals who have made valuable contributions to the
project through code, documentation, bug reports, security reports,
testing, reviews, or other meaningful contributions.

Listed alphabetically.

- **jmg2248** ([@jmg2248](https://github.com/jmg2248)) - Reported and
  analyzed memory-safety issues: heap buffer over-read in `to_string`
  (Issue [#12](https://github.com/nedanwr/kronos/issues/12)),
  use-after-free in `gc_cleanup` during list deallocation (Issue
  [#17](https://github.com/nedanwr/kronos/issues/17)), and heap
  use-after-free in `patch_pending_jumps` due to unsafe early return
  (Issue [#18](https://github.com/nedanwr/kronos/issues/18))

- **ksavvb** ([@ksavvb](https://github.com/ksavvb)) - Implemented
  `read_file()` built-in function and fixed heap buffer over-read in
  `to_string` (PR [#22](https://github.com/nedanwr/kronos/pull/22),
  closes [#12](https://github.com/nedanwr/kronos/issues/12))

## How to Contribute

We welcome contributions from the community! If you'd like to contribute
to Kronos:

1. Check out our [Contributing Guidelines](.github/CONTRIBUTING.md)
2. Open an issue to discuss your contribution
3. Submit a pull request with your changes

Contributors will be recognized here for their valuable contributions to
the project.

---

**Note:** This file is maintained manually. If you've contributed to
Kronos and would like to be recognized here, please open an issue or
pull request adding yourself to the Contributors list (include your
name/handle and a short description of what you did).
