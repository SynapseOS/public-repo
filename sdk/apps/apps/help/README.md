# help

`help` lists installed topics; `help setup` reads
`/usr/share/help/setup.md`. `help -f FILE` renders a Markdown file.
`help --plain TOPIC` suppresses ANSI styles. Piped output, `TERM=dumb`,
and `NO_COLOR` also use plain text.

Pages may contain UTF-8 text, headings, lists, block quotes, inline code,
emphasis, links, and fenced or indented code blocks. Code remains literal;
terminal control characters from page content are stripped. This is a compact
reader, not a complete CommonMark implementation. It does not require a pager.

Packages can include `usr/share/help/NAME.md` as an ordinary manifest file
with mode `0644`; installing or removing the package installs or removes its
help page through the existing setup file ownership rules.

Build with `make -C apps/apps help/` from the project root. This uses the
project's static musl linker. Run `python3 tests/help-smoke.py` for host tests.
