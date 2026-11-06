#!/usr/bin/env python3
"""Add base-station rows to the Cats dashboard.

The dashboard is a `sections` view: one grid section per cat, each grid carrying a
heading (cat), a subtitle (base), and four tiles (in range / last seen / signal /
battery). This appends the same block for a base station, so a base reads as one more
row per cat, and adds a gateway-status section.

Entity ids are taken from the entity registry by unique_id (`base3_n0_cat1_signal`),
never typed: the ids Home Assistant generates for `Cat Base 3` collided with the
first, buggy discovery pass and now carry numeric suffixes, so an id typed by hand is
not the id in the registry.

    python3 tools/cats_dashboard.py --base base3                  # print, no write
    python3 tools/cats_dashboard.py --base base3 --write          # save
    python3 tools/cats_dashboard.py --base base3 --node 1 --write  # also add relay-node rows

`HA_TOKEN` must be a long-lived token (admin), because `lovelace/config/save` is.
"""
import asyncio
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ha_registry import HA  # noqa: E402  same directory

DASH = "dashboard-cats"
CATS = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "..", "cats.json")))


def tiles(mapping, uid_prefix, cat, names):
    out = []
    for field, label, icon in names:
        uid = f"{uid_prefix}_{field}"
        eid = mapping.get(uid)
        if eid is None:
            print(f"  [skip] no entity for {uid}", file=sys.stderr)
            continue
        out.append({"type": "tile", "entity": eid, "name": label, "icon": icon})
    return out


CAT_TILES = [
    ("in_range", "in range", "mdi:access-point-network"),
    ("last_seen", "last seen", "mdi:clock-outline"),
    ("signal", "signal", "mdi:signal-variant"),
    ("battery", "battery", "mdi:battery"),
]

BASE_TILES = [
    ("battery", "battery", "mdi:battery"),
    ("charging", "charging", "mdi:lightning-bolt"),
    ("wifi_signal", "wifi signal", "mdi:wifi"),
    ("packets", "packets", "mdi:email"),
    ("unknown_id_drops", "unknown ids", "mdi:alert"),
]

NODE_TILES = [
    ("packets", "packets", "mdi:email"),
    ("missed", "missed", "mdi:alert"),
    ("in_range", "node heard", "mdi:access-point"),
    ("last_seen", "last heard", "mdi:clock-outline"),
]


def build(mapping, base, label, nodes):
    """Cards to append, keyed off the registry mapping."""
    blocks = []
    for cid, cat in sorted(CATS.items()):
        head = {"type": "heading", "heading": f"{base} · {label}",
                "heading_style": "subtitle"}
        block = {"type": "heading", "heading": cat, "cards": []}
        block["cards"].append(head)
        block["cards"] += tiles(mapping, f"{base}_n0_cat{cid}", cat, CAT_TILES)
        blocks.append(block)
    return blocks


def status_section(base, mapping, nodes):
    cards = [{"type": "heading", "heading": f"{base} · gateway status",
              "heading_style": "title"}]
    cards += tiles(mapping, f"{base}", base, BASE_TILES)
    for n in nodes:
        cards.append({"type": "heading", "heading": f"node {n}",
                      "heading_style": "subtitle"})
        cards += tiles(mapping, f"{base}_n{n}", f"node {n}", NODE_TILES)
    return {"type": "grid", "cards": cards}


async def main():
    base = sys.argv[sys.argv.index("--base") + 1]
    label = sys.argv[sys.argv.index("--label") + 1] if "--label" in sys.argv else "gateway"
    write = "--write" in sys.argv
    nodes = []
    if "--node" in sys.argv:
        # Stop at the next flag:  must not try to int() the flag.
        for x in sys.argv[sys.argv.index("--node") + 1:]:
            if x.startswith("--"):
                break
            nodes.append(int(x))

    ha = HA()
    await ha.connect()
    ents = await ha.call("config/entity_registry/list")
    mapping = {e["unique_id"]: e["entity_id"] for e in ents if e.get("unique_id")}
    cfg = await ha.call("lovelace/config", url_path=DASH)
    view = cfg["views"][0]
    sections = view["sections"]

    # A section is identified by its first heading text, so re-running does not add
    # the same base twice.
    marker = f"{base} · {label}"
    for s in sections:
        for c in s.get("cards", []):
            if c.get("heading") == marker:
                print(f"{base} already on the dashboard, nothing to do")
                await ha.close()
                return

    blocks = build(mapping, base, label, nodes)
    for s in sections:
        cards = s.get("cards", [])
        if not cards:
            continue
        cat = cards[0].get("heading")
        hits = [b for b in blocks if b["heading"] == cat]
        if hits:
            cards.extend(hits[0]["cards"])
    sections.append(status_section(base, mapping, nodes))

    print(json.dumps(sections[-1], indent=1)[:1200])
    print(f"added {marker} to {len(blocks)} cat sections")
    if write:
        await ha.call("lovelace/config/save", url_path=DASH, config=cfg)
        print("saved", DASH)
    else:
        print("not written (add --write)")
    await ha.close()


asyncio.run(main())
