# Parity with android-diagnoser

The C++ core reproduces android-diagnoser (commit 7b2d820) exactly, quirks
included, so the two can be compared on the same capture before either is
changed. These are the quirks found while porting. Each shows in the golden
files; the ones marked *checked* were also looked at by hand after the merge.

1. **Indented package dumps: fixed, parity left on purpose.**
   android-diagnoser reads a package header only at column 0 and a section
   name only at exactly two spaces. A real phone indents the header by two
   spaces: in a capture from a Pixel 8 Pro on Android 17 (2026-11-16) all 549
   headers were indented and none at column 0, and both programs gave the
   same report (JSON and text identical) with 0 granted permissions, 0
   trackers, 0 versions and 0 apps with internet permission across 443 app
   cards. Snoopy now reads an indented dump relative to its headers, skips
   packages under "Hidden system packages" (the factory copies of updated
   system apps), and adds each app's components from the resolver tables
   (where a real dump lists them, never in the Package block) to the text
   the tracker search reads. On the same capture: 573 granted sensitive
   permissions, 222 trackers in 127 apps, versions for 427 apps, internet
   permission for 219. A dump with any header at column 0 is read exactly
   as before, so every other golden file still matches; `real-indent`'s
   golden files keep the old reading and `packageDumpReadsIndented` /
   `packageDumpLikeAPhone` check the new one.
2. **DHCP filter by text** (*checked*, `android14`). Destinations whose kind
   contains "port 67" or "port 68" are dropped from the graph, so port 6789
   is dropped too: it is in `flows.json`, not in `graph.json`.
3. **Netstats order.** Uids from the netstats history come in the order
   CPython 3.14 iterates a set; the port reproduces that order.
4. **Nested permissions.** Runtime permissions nested under "User 0:" count
   toward the previous section, so they are treated as granted.
5. **Duplicate package entries.** A later entry (for example under "Hidden
   system packages") replaces the earlier details.
6. **Job names.** A job line with a hex job id is named by that id
   ("Scheduled job 5ab9c1d").
7. **Old-format times.** Times like "+1d2h ago" have no "(-", so reads older
   than a day stay on the graph, labelled "in this sample".
8. **Shared uids in the graph.** Reads attach to the op's package and
   connections to the app name, so one shared uid can become separate cards.
9. **One-line app-op modes.** "mode=allow" in the one-line format is not read
   as a mode ("mode unrecorded"); a line with no package or an unreadable
   uid is dropped.
10. **Clipboard owner.** The first match wins.

Where C++ may still differ from Python, and no fixture shows it: the order
of "Capture gaps" when meta.json is not sorted (Qt sorts JSON keys), how
many replacement characters invalid UTF-8 becomes, inputs on which Python
raises (C++ carries on), and Unicode corner cases in the patterns.

Before item 1 was fixed, the Pixel 8 Pro capture gave identical reports
from both programs, so the port matched on real data too. The other items
are kept until a real capture shows they matter.

## Icons (icons.py)

Checked on the same Pixel 8 Pro (Android 17, toybox 0.8.13), 2026-11-16:

- **Reads.** `dd bs=1` became `bs=65536 iflag=skip_bytes,count_bytes`
  (200,000 bytes: 112 ms against 387 ms, same bytes), falling back to `bs=1`
  for a phone that refuses the flag; a lookup no longer reads the same tail,
  directory or size twice.
- **Large tables.** The on-phone walker took 12,176 ms on YouTube's table and
  returned nothing; with fast reads, tables up to 20,000,000 bytes are now
  copied (8,000,000 bytes in 676 ms) and indexed here. The walker only runs
  when the phone reads one byte per block.
- **Sparse type chunks** (bug in icons.py). A sparse chunk's entry count is
  its number of pairs, not the highest index; icons.py rejected any entry
  index at or above it, so YouTube's icon (entry 15 of a 5-pair chunk) was
  never found.
- **Shrunk resource paths.** Bitmaps named without an extension (Edge:
  `res/HxS`, `res/w5-`) are accepted; the bytes must still be PNG or WebP.

Five apps before and after these changes: 16,251 to 72,728 ms per icon with
3 of 5 missing, then 2,602 to 3,353 ms with 1 missing (Outlook, whose icon
is vector XML only). Over 20 third-party apps from one sample: 10 real icons,
8 vector-only (all-XML chains, no bitmap to show), 2 with no icon attribute
found in the manifest (cause not known); 710 to 6,533 ms each, mean 2,273 ms.
