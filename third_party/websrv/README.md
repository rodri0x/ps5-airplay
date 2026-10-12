# websrv

Unmodified `src/ps5/pt.c`, `pt.h`, `elfldr.c` and `elfldr.h` from
<https://github.com/ps5-payload-dev/websrv>, commit
`7748df9d2a43822a5ebfda149f43d4f2696d640b`. Copyright (C) John Törnblom, GPLv3 (see `LICENSE`).

`pt.c` drives another process with ptrace and `elfldr.c` loads an ELF into it. They run
`display_host.elf` inside the app that `src/display_carrier.c` starts.
