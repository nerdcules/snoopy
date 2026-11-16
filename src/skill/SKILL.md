---
name: snoopy
description: Read what each app on a USB-connected Android phone recently read (location, camera, microphone, contacts...) and where it is connected, through the snoopy command. Use when asked which apps on the phone track location, use the camera or microphone, talk to trackers, or connect to which servers.
---

# snoopy

`snoopy` reads a phone connected over USB with adb (USB debugging on, this
computer accepted on the phone). Commands exit 0 on success and 1 with the
problem on stderr (no adb, no phone, phone not authorized, two phones: pass
`--serial`).

- `snoopy scan --json`: one sample as a report. `live` holds one card per
  Linux uid with `packages`, `observed` (sensitive operations the phone
  recorded, with `category`, `op`, `last_access`), `attempted` (rejected),
  `permitted_unseen`, `declared` (granted permissions), `trackers` (SDKs
  shipped in the app and what they collect), `doing` (activities, services,
  sensors), and `transmitted` (`connections` with remote address, proto and
  state; `dns` lookups; `tx_bytes`/`rx_bytes`). Also `declared_only`,
  `unattributed_dns`, `device`, `limits`, `errors`.
- `snoopy scan --out DIR`: also saves the raw dumps, for
  `snoopy report --capture DIR [--json]` later, offline.
- `snoopy scan --package com.example.app`: one app (repeatable). `--all`
  includes apps with nothing live.
- `snoopy watch --interval 5 --count 12`: prints sockets, focus and app ops
  as they change.
- `snoopy check --json [--all]`: findings worth a look, each with `level`
  (alert, warn, info), `group` (background, control, network, outside,
  device), `package`, `title`, `detail`, `ago` (seconds, for background
  use) and `new` (not in the phone's baseline). Background use is sensitive
  data accessed while the app wasn't on screen; control is accessibility,
  notification access, device admin and similar; outside is apps not from
  Google Play. A finding is a reason to look, not proof.

If `snoopy.service` runs, `~/.local/state/snoopy/status.json` holds the
last sample's summary (connected, transport, model, alerts, warnings,
newCount, updatedAt) without touching the phone.

What the data can and can't say:
- A socket belongs to a Linux uid; on Android that is usually one app.
- HTTPS and QUIC bodies stay encrypted: the address, port and byte totals
  are visible, the contents are not. Nothing is decrypted.
- A tracker entry means the SDK is inside the app, not that it sent anything.
- A destination's name is its reverse DNS record, not necessarily the name
  the app looked up; `dns` entries are names the phone itself resolved.
- Byte totals are the phone's per-uid counters since they last reset, not
  one request.
