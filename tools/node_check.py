#!/usr/bin/env python3
"""Is the node in the tree talking to the base right now?

    python3 tools/node_check.py                # one snapshot + verdict
    python3 tools/node_check.py --watch 120    # poll every 5 s until a node reports
    python3 tools/node_check.py base3 --nodes 1,2

The answer comes from Home Assistant, not from the node. base3 publishes its node
rows by discovery (`cattracker/base3/n1/...`), so `base3_n1_in_range` is the house's
own record of having heard node 1. That makes this a **connection check you can run at
the base of a tree with a laptop on the house WiFi**, with nothing added to the node
and no cable: raise the node, run it, and within one 8 s report cycle the row says
whether the house heard it.

Entity ids are looked up from the entity registry by **unique_id**, never typed. The
first discovery pass generated ids with numeric suffixes
(`binary_sensor.cat_base_3_node_1_node_3_in_range_4`) and those ids survive deleting
entities one by one, because the discovery record remembers its suggested ids. A
hand-typed id is a coin flip. See `tools/ha_registry.py`.

Registry over the WebSocket API, states over REST: this install answers **no** state
WebSocket command (`get_state`, `state/get`, `state/get_all` all return
`unknown_command`), so states have to come from `/api/states/<entity_id>`. Needs a
**long-lived** token in `HA_TOKEN` — a token issued before HA rotated its signing keys
gets `401`/`auth_invalid` on both APIs.

Verdicts, in the order they are worth reading:

- `in_range on`, `last seen` under 30 s — the node is transmitting and the base heard it.
- `in_range off` with packets > 0 — reporting stopped: battery/solar, antenna, or the
  base went deaf. Node battery is not published yet (TP5000 unwired), so the node
  cannot tell you which.
- `packets` unchanged between two polls — the node is not transmitting at all.
- `missed > 0` — gaps in the node's own seq counting: marginal link, not a dead node.

Exit code is 0 if any node is reporting, 1 if none, so this is usable as the pass/fail
gate of a field checklist.
"""
import asyncio
import json
import os
import re
import sys
import urllib.request
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ha_registry import HA  # noqa: E402 - same directory

BASE_URL = os.environ.get("HA_URL", "http://192.168.100.66:8123")
TOKEN = os.environ.get("HA_TOKEN", "")
NODE_UID = re.compile(r"^(.*)_n(\d+)_(in_range|last_seen|packets|missed|battery|charging)$")
IN_RANGE_WINDOW = 30  # a node reports every 8 s; 30 s is one lost report
# LiFePO4 bottom is ~3.0 V. Below 3.10 V the cell is past the flat part of its curve
# and minutes, not hours, of runtime are left, so this is the alarm, not a percentage.
LOW_V = 3.10


def rest_get(path):
    """REST read with the long-lived token. Raises on 401, which is the token, not
    the entity."""
    req = urllib.request.Request(BASE_URL + path,
                                 headers={"Authorization": "Bearer " + TOKEN})
    return json.loads(urllib.request.urlopen(req, timeout=10).read())


def node_entities(ents):
    """{(node, field): entity_id} from unique_id `base3_n1_in_range`.

    Per-collar rows (`base3_n1_cat0_signal`) do not match the pattern, so they are
    skipped by the regex rather than by a filter.
    """
    out = {}
    for e in ents:
        m = NODE_UID.match(e.get("unique_id") or "")
        if not m:
            continue
        out[(int(m.group(2)), m.group(3))] = e["entity_id"]
    return out


def age_seconds(s):
    """Age of a HA timestamp string in seconds, or None if it is not one.

    `unavailable`/`unknown` are valid entity states and have no time to subtract, so
    the string is tested rather than assumed.
    """
    try:
        dt = datetime.fromisoformat(s)
    except (ValueError, TypeError):
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return max(0, int((datetime.now(timezone.utc) - dt).total_seconds()))


def snapshot(nodes, uids):
    rows = []
    for node in nodes:
        row = {}
        for field in ("in_range", "last_seen", "packets", "missed", "battery", "charging"):
            eid = uids.get((node, field))
            row[field] = None if eid is None else rest_get(f"/api/states/{eid}")["state"]
        rows.append((node, row))
    return rows


def line(node, row):
    return (f"  node {node}: in_range={row.get('in_range')} "
            f"last_seen={row.get('last_seen')} packets={row.get('packets')} "
            f"missed={row.get('missed')} cell={row.get('battery')} V "
            f"charging={row.get('charging')}")


def verdict(node, row, prev_pk):
    ir = row.get("in_range")
    age = age_seconds(row.get("last_seen"))
    pk = row.get("packets")
    try:
        volts = float(row.get("battery"))
    except (ValueError, TypeError):
        volts = None
    if volts is not None and volts < LOW_V:
        # The alarm is on volts, not on a percentage: a LiFePO4 curve is flat, so the
        # percentage that reads "fine" at 3.05 V is the lie, and the volts are the fact.
        return f"node {node}: **cell low** - {volts} V (bottom is ~3.0 V)", False
    if ir == "on" and age is not None and age <= IN_RANGE_WINDOW:
        return f"node {node}: **reporting** - base heard it {age} s ago", True
    if pk is not None and prev_pk is not None and str(pk) == str(prev_pk):
        return f"node {node}: **not transmitting** - packets {pk} unchanged since last poll", False
    if ir == "off":
        seen = f"{age} s ago" if age is not None else str(row.get("last_seen"))
        return (f"node {node}: **not heard** - last seen {seen}, packets {pk}"), False
    return f"node {node}: in_range={ir} last_seen={row.get('last_seen')} packets={pk}", False


async def main():
    args = sys.argv[1:]
    watch = 0
    if "--watch" in args:
        i = args.index("--watch")
        watch = int(args[i + 1])
        del args[i:i + 2]
    nodes = None
    if "--nodes" in args:
        nodes = [int(x) for x in args[args.index("--nodes") + 1].split(",")]
        del args[args.index("--nodes"):args.index("--nodes") + 2]
    base = args[0] if args else "base3"

    ha = HA()
    await ha.connect()
    ents = await ha.call("config/entity_registry/list")
    await ha.close()

    uids = node_entities(ents)
    present = sorted({n for (n, f) in uids if n >= 1 and f == "in_range"})
    if not present:
        print(f"no node rows for {base} in the registry - the base is not publishing "
              "discovery. Check base3's console (key v for verbose).")
        return 1
    if nodes is None:
        nodes = present

    prev = {}
    reporting = set()
    while True:
        rows = snapshot(nodes, uids)
        for node, row in rows:
            print(line(node, row))
            v, ok = verdict(node, row, prev.get(node))
            print(f"  => {v}")
            prev[node] = row.get("packets")
            if ok:
                reporting.add(node)
            elif node in reporting:
                reporting.discard(node)
        sys.stdout.flush()
        if watch <= 0:
            break
        await asyncio.sleep(5)
        watch -= 5
    print()
    if reporting:
        print(f"nodes reporting to {base}: {sorted(reporting)}")
    else:
        print(f"NO node reporting to {base} - node not transmitting, or {base} deaf. "
              "Check node power/antenna, then base3 console key v.")
    return 0 if reporting else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
