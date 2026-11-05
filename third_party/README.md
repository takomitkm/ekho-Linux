# third_party

Code that is vendored into this tree so the build needs nothing from the system
that the build host cannot supply. Each entry says where it came from and under
what licence, because the licences of vendored code have to travel with it.

## What is here, and why

| component | version / origin | licence | why it is vendored |
|---|---|---|---|
| `sonic/` | libsonic (Bill Cox), `sonic.c` + `sonic.h` | Apache-2.0 | The board has no libsonic at all, and the speech-rate stage needs it. `Makefile.am` builds it from this tree (`noinst_LIBRARIES = libekho.a libsonic.a`). The licence notice is in the first eight lines of `sonic.c` and `sonic.h`. |
| `alsa/include/alsa/` | alsa-lib headers, `SND_LIB_VERSION_STR "1.2.15.3"` | LGPL-2.1-or-later | The board ships `libasound.so.2` but **no `libasound2-dev`**, so there are no headers to compile against. Every file carries its own licence notice at the top. |
| `utf8cpp/` | utf8cpp 4.0.9 (Nemanja Trifunovic), `utf8.h` + `utf8/*.h` | BSD-2-Clause (notice in `utf8.h`) | `src/ssml.cpp` walks UTF-8 through it and `src/character.h` includes it, so every target needs the header; a fresh host otherwise needs `libutfcpp-dev` (and the board kit used to fetch it from a local MSYS2 tree, `cross/x-kit-ekho.sh`). Vendored so the documented dependency list -- libsndfile and espeak-ng only -- stays true. The cross kit keeps its own copy and its own include path. |

Neither of these is resolved against a system library at link time: libsonic is
compiled from this tree into the program's own static archive, and the ALSA
headers only let `src/alsa_backend.cpp` name the right symbols -- libasound
itself is `dlopen`ed at run time (`snd_pcm_open`, `snd_pcm_set_params`,
`snd_pcm_writei`, ...). That is why the executables have no `libasound.so.2` in
DT_NEEDED.

Note on the ALSA headers: they are **newer** than the library the board runs
(1.2.15.x headers against the board rootfs's `libasound.so.2.0.0`; the exact
runtime version was not measured, the rootfs inventory records file names only).
That is deliberate and safe for the subset used here -- the functions this tree
calls (`snd_pcm_open`, `snd_pcm_close`, `snd_pcm_set_params`, `snd_pcm_writei`,
`snd_pcm_drain`, `snd_pcm_prepare`, `snd_pcm_recover`, plus optional
`snd_pcm_avail_delay`, `snd_strerror`, `snd_pcm_name`, `snd_pcm_format_name`)
have been part of the ABI since the 0.x days, and the `snd_pcm_*` enums are
frozen. Anything beyond that subset would need a symbol check against the
target.

## Licence of this tree

ekho itself is **GPL-2.0-or-later** (see `COPYING` and `LICENSE`; every source
file carries the notice). That matters for the libraries that get linked in
statically:

| linked in | licence | note |
|---|---|---|
| espeak-ng | GPL-3.0-or-later | `or-later` on both sides, so there is no GPL-2-only/3 conflict. |
| libsndfile | LGPL-2.1-or-later | Static linking means the LGPL's relink requirement applies; the corresponding source and the build scripts are published with the binaries, which is what satisfies it. |
| libsonic | Apache-2.0 | GPL-compatible. |
| alsa-lib | LGPL-2.1-or-later | Compiled against headers only; no ALSA code in the binary. |

So the combination is **GPL-3.0-or-later**, not GPL-2: espeak-ng and the files
this fork adds (`src/alsa_backend.*`, `src/streaming.*`, `src/http_server.*`,
`src/text_normalize.*`, `src/ekho_stream_main.cpp`, `src/ekho_server_main.cpp`)
are GPL-3.0-or-later, and both licences are "or later", so the stricter one
governs the binary. The ekho files inherited from upstream stay available under
GPL-2.0-or-later as well. Handing somebody a binary means handing them the
corresponding source, the licence texts, and a note of what was changed (this
repository is that source).

Full licence texts for the linked libraries are not duplicated here; they are in
the upstream distributions and, for a built kit, under `share/doc/` next to the
binaries.
