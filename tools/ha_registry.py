#!/usr/bin/env python3
"""Home Assistant registry and dashboard edits over the WebSocket API.

The registries and the storage dashboards have no REST endpoints, so everything here
is a WebSocket command. The command names below were read out of the Home Assistant
source on `m710q`, not guessed:

    config/device_registry/list          config/device_registry/remove_config_entry
    config/entity_registry/list          config/entity_registry/remove
    lovelace/config                      lovelace/config/save

`lovelace/config/save` is admin-only, and `config/entity_registry/remove` is the way
to clear a base station's entities: the unique ids are index-based, so re-publishing
discovery renames entities in place and the colliding `entity_id` keeps the numeric
suffix Home Assistant adds on a collision (`sensor.cat_base_3_node_1_node_3_signal_2`).
Removing the entities leaves nothing to collide with, and the next discovery pass
creates clean ones.

Token: `HA_TOKEN` (long-lived, from User profile -> Permissions -> Create Token).
A token issued before Home Assistant rotated its signing keys returns
`auth_invalid` for the WebSocket too, not just for REST.

    python3 tools/ha_registry.py devices                  # every device
    python3 tools/ha_registry.py remove-device base3        # remove device + its discovery entries
    python3 tools/ha_registry.py devices base3            # filter by identifier
    python3 tools/ha_registry.py entities base3           # entities by unique_id
    python3 tools/ha_registry.py remove-entities base3    # remove them (writes)
    python3 tools/ha_registry.py dashboard dashboard-cats # print the config
    python3 tools/ha_registry.py save-dashboard dashboard-cats file.json
"""
import asyncio
import json
import os
import sys

import websockets

URL = os.environ.get("HA_WS", "ws://192.168.100.66:8123/api/websocket")
TOKEN = os.environ.get("HA_TOKEN", "")


class HA:
    def __init__(self):
        self.ws = None
        self.rid = 0

    async def connect(self):
        self.ws = await websockets.connect(URL, max_size=None)
        first = json.loads(await self.ws.recv())
        if first.get("type") != "auth_required":
            raise SystemExit(f"unexpected first message: {first}")
        await self.ws.send(json.dumps({"type": "auth", "access_token": TOKEN}))
        ok = json.loads(await self.ws.recv())
        if ok.get("type") != "auth_ok":
            raise SystemExit(f"auth failed: {ok} - the token is not valid (long-lived "
                             "token from User profile -> Permissions)")

    async def call(self, typ, **kw):
        self.rid += 1
        await self.ws.send(json.dumps({"id": self.rid, "type": typ, **kw}))
        while True:
            m = json.loads(await self.ws.recv())
            if m.get("id") == self.rid:
                if not m.get("success"):
                    raise SystemExit(f"{typ} failed: {m.get('error')}")
                return m["result"]

    async def close(self):
        await self.ws.close()


async def main():
    cmd = sys.argv[1]
    arg = sys.argv[2] if len(sys.argv) > 2 else ""
    ha = HA()
    await ha.connect()

    if cmd == "remove-device":
        r = await ha.call("config/device_registry/list")
        devs = r.get("devices", []) if isinstance(r, dict) else r
        hit = [d for d in devs if arg in " ".join(i[1] for i in d.get("identifiers", []))]
        for d in hit:
            # Removing the config entry is what clears the MQTT discovery cache: the
            # entity_id suffixes survive removing entities one by one, because the
            # discovery record itself still remembers its suggested ids.
            for ce in d.get("config_entries", []):
                await ha.call("config/device_registry/remove_config_entry",
                              config_entry_id=ce)
            print("removed config entries for", d.get("name_by_user") or d.get("name"))

    elif cmd == "devices":
        r = await ha.call("config/device_registry/list")
        devs = r.get("devices", []) if isinstance(r, dict) else r
        for d in devs:
            ids = " ".join(i[1] for i in d.get("identifiers", []))
            if arg and arg not in ids:
                continue
            print(d["id"], "|", d.get("name_by_user") or d.get("name"), "|", ids)

    elif cmd == "entities":
        ents = await ha.call("config/entity_registry/list")
        for e in ents:
            uid = e.get("unique_id") or ""
            if arg and arg not in uid:
                continue
            print(e["entity_id"], "|", uid, "|", e.get("device_id"), "|", e.get("disabled_by"))

    elif cmd == "remove-entities":
        ents = await ha.call("config/entity_registry/list")
        hit = [e for e in ents if arg in (e.get("unique_id") or "")]
        print(f"removing {len(hit)} entities matching {arg}")
        for e in hit:
            await ha.call("config/entity_registry/remove", entity_id=e["entity_id"])
        print("done. Republish discovery (restart the base station) to recreate them.")

    elif cmd == "dashboard":
        cfg = await ha.call("lovelace/config", url_path=arg)
        print(json.dumps(cfg, indent=2))

    elif cmd == "save-dashboard":
        cfg = json.load(open(arg))
        await ha.call("lovelace/config/save", url_path=sys.argv[3], config=cfg)
        print("saved", sys.argv[3])

    await ha.close()


if __name__ == "__main__":
    asyncio.run(main())
