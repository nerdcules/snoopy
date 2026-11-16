# Parity with android-diagnoser

The C++ core reproduces android-diagnoser (commit 7b2d820) exactly, quirks
included, so the two can be compared on the same capture before either is
changed. These are the quirks found while porting. Each shows in the golden
files; the ones marked *checked* were also looked at by hand after the merge.

1. **Indented package dumps** (*checked*, `real-indent`). A package header is
   read only at column 0 (`Package [x]`) and a section name only at exactly
   two spaces. With the header indented by two spaces, no grants, trackers or
   versions are read: in `golden/real-indent/report.json` both apps have
   `declared` 0, `trackers` 0 and empty `versions`. If real phones indent the
   header (expected for AOSP's `dumpsys package`, not yet seen on a device),
   every app's granted permissions, tracker SDKs and versions are missed.
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

Next: a capture from a real phone, run through both programs, decides which
of these are bugs worth leaving parity for. Item 1 first.
