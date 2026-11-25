#!/usr/bin/env python3
"""Validate captured discovery messages with Home Assistant's own schemas.

  uv run --python 3.14 --with homeassistant --with paho-mqtt tests/check_ha.py discovery.json
"""
import asyncio, collections, importlib, json, sys, tempfile
from homeassistant.components.mqtt import discovery as disco
from homeassistant.helpers.template import Template

from homeassistant.core import HomeAssistant


async def main():
    hass = HomeAssistant(tempfile.mkdtemp())        # templates need a running instance
    msgs = json.load(open(sys.argv[1]))
    count, bad = collections.Counter(), []
    for topic, payload in sorted(msgs.items()):
        _, comp, node, obj, last = topic.split("/")
        assert last == "config" and disco.TOPIC_MATCHER.match(topic.removeprefix("homeassistant/")), topic
        p = disco.MQTTDiscoveryPayload(payload) if hasattr(disco, "MQTTDiscoveryPayload") else dict(payload)
        try:
            disco._replace_all_abbreviations(p)
            if "~" in p:
                disco._replace_topic_base(p)
            mod = importlib.import_module(f"homeassistant.components.mqtt.{comp}")
            out = mod.DISCOVERY_SCHEMA(dict(p))
            tpl = out.get("value_template")
            if tpl is not None:
                assert tpl.template.startswith("{{value_json.") or tpl.template.startswith(("{{'ON' if", "{{'no parity' if")), tpl
            unknown = set(p) - set(out) - {"~"}
            if unknown:
                bad.append((topic, f"ignored keys: {unknown}"))
            assert out["device"]["identifiers"] == [node]
            count[comp] += 1
        except Exception as e:
            bad.append((topic, f"{type(e).__name__}: {e}"))
    for t, e in bad:
        print("BAD", t, e)
    print("valid:", dict(count), "total", sum(count.values()), "of", len(msgs))
    return 1 if bad else 0


code = asyncio.run(main())
sys.exit(code)
