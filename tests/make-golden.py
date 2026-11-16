#!/usr/bin/env python3
"""Write the golden outputs the C++ port is compared with.

Rerun after changing a fixture, from the repo root:

    python3 -I tests/make-golden.py <android-diagnoser checkout> tests/fixtures

<android-diagnoser checkout> is the directory holding the android_diagnoser
package (the reference is commit 7b2d820). Every directory under the fixtures
directory except golden/ is a capture, in the layout `scan --out` writes. For
each one this writes tests/fixtures/golden/<capture>/:

    report.json         build_report(capture)
    report-all.json     build_report(capture, include_all=True)
    report-filter.json  build_report(capture, packages_filter={FILTERS[capture]})
    flows.json          project_report(report.json)
    graph.json          graph_from(flows, apps), names.cached_name returning ""
    text.txt            render_text(report.json)
    text-all.txt        render_text(report-all.json)
    text-filter.txt     render_text(report-filter.json)
    watch.json          watch_snapshot(capture)
    manifest.json       {"filter": <the package report-filter.json kept>}

and for each (before, after) pair in WATCH_PAIRS, golden/watch/<before>--<after>/:

    before.json, after.json   watch_snapshot of each capture
    diff.txt                  diff_watch(before, after), one event per line
    diff-reverse.txt          diff_watch(after, before)

JSON is written with json.dumps(sort_keys=True, indent=2).
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

# The package report-filter.json keeps, per capture. A capture not listed
# keeps the first package of its pm list.
FILTERS = {
    "demo": "com.quiet.notes",
    "android14": "com.whatsapp",
    "android17": "com.a.one",
    "legacy": "com.old.messenger",
    "real-indent": "com.example.indented",
    "empty": "com.example.absent",
    "watch-before": "com.watch.one",
    "watch-after": "com.watch.two",
}

WATCH_PAIRS = [
    ("watch-before", "watch-after"),
    ("demo", "android14"),
]


def dump(path: Path, value) -> None:
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    sys.path.insert(0, str(Path(sys.argv[1]).resolve()))
    fixtures = Path(sys.argv[2]).resolve()

    from android_diagnoser import summary
    from android_diagnoser.analyze import build_report, diff_watch, watch_snapshot
    from android_diagnoser.collect import load_capture
    from android_diagnoser.flows import project_report
    from android_diagnoser.parsers import parse_pm_list
    from android_diagnoser.render import render_text

    summary.cached_name = lambda host: ""

    golden = fixtures / "golden"
    if golden.exists():
        shutil.rmtree(golden)
    golden.mkdir()

    captures = sorted(path for path in fixtures.iterdir() if path.is_dir() and path.name != "golden")
    snapshots = {}
    for directory in captures:
        capture = load_capture(directory)
        out = golden / directory.name
        out.mkdir()
        wanted = FILTERS.get(directory.name)
        if wanted is None:
            listed = parse_pm_list(capture.packages)
            wanted = listed[0].name if listed else ""
        report = build_report(capture)
        everything = build_report(capture, include_all=True)
        filtered = build_report(capture, packages_filter={wanted})
        dump(out / "report.json", report)
        dump(out / "report-all.json", everything)
        dump(out / "report-filter.json", filtered)
        projected = project_report(report)
        dump(out / "flows.json", projected)
        dump(out / "graph.json", summary.graph_from(projected["flows"], projected["apps"]))
        (out / "text.txt").write_text(render_text(report), encoding="utf-8")
        (out / "text-all.txt").write_text(render_text(everything), encoding="utf-8")
        (out / "text-filter.txt").write_text(render_text(filtered), encoding="utf-8")
        snapshots[directory.name] = watch_snapshot(capture)
        dump(out / "watch.json", snapshots[directory.name])
        dump(out / "manifest.json", {"filter": wanted})
        print(f"golden/{directory.name}")

    for before, after in WATCH_PAIRS:
        out = golden / "watch" / f"{before}--{after}"
        out.mkdir(parents=True)
        dump(out / "before.json", snapshots[before])
        dump(out / "after.json", snapshots[after])
        forward = diff_watch(snapshots[before], snapshots[after])
        backward = diff_watch(snapshots[after], snapshots[before])
        (out / "diff.txt").write_text("".join(line + "\n" for line in forward), encoding="utf-8")
        (out / "diff-reverse.txt").write_text("".join(line + "\n" for line in backward), encoding="utf-8")
        print(f"golden/watch/{before}--{after}: {len(forward)} and {len(backward)} events")
    return 0


if __name__ == "__main__":
    sys.exit(main())
