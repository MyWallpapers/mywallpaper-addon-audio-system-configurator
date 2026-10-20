"""Validate outputs emitted by the compiled provider, without running old tests."""
import base64
import hashlib
import json
import sys
from pathlib import Path

from audio_system_contract_test import compact, validate, validate_snapshot, validate_state


def main(path):
    envelopes = json.loads(Path(path).read_text())
    inventories = {}
    kinds = set()
    maximum = 0
    for envelope in envelopes:
        frame = {"v": 5, "type": "message", "target": "broadcast", "payload": envelope}
        size = len(compact(frame))
        assert size <= 65536, size
        maximum = max(maximum, size)
        assert envelope["type"] == "mywallpaper.services" and envelope["v"] == 1
        if envelope["kind"] != "packet":
            continue
        packet = envelope["packet"]
        value = packet.get("value")
        if value is None:
            continue
        validate(value)
        kinds.add(value["kind"])
        if value["kind"] == "audio-system.state":
            validate_state(value)
        elif value["kind"] == "audio-system.snapshot.result":
            validate_snapshot(value)
        elif value["kind"] == "audio-system.inventory.result":
            key = (value["generation"], value["topic"], value["revision"], value["inventoryId"], value["sha256"])
            inventory = inventories.setdefault(key, {"parts": {}, "total": value["totalBytes"]})
            data = base64.b64decode(value["data"], validate=True)
            assert len(data) <= 32768
            assert value["nextOffset"] == (None if value["offset"] + len(data) == value["totalBytes"] else value["offset"] + len(data))
            inventory["parts"][value["offset"]] = data
    complete = 0
    for key, inventory in inventories.items():
        data = b""
        for offset, part in sorted(inventory["parts"].items()):
            assert offset == len(data), "Gap or overlap in native continuation"
            data += part
        assert len(data) == inventory["total"]
        assert "sha256:" + hashlib.sha256(data).hexdigest() == key[-1]
        value = json.loads(data.decode("utf-8"))
        validate(value, key[1] + "Value")
        validate_state({"kind": "audio-system.state", "topic": key[1], "generation": key[0], "revision": key[2],
                        "availability": "ready", "error": None, "body": {"mode": "inline", "value": value}}, bounded=False)
        assert len(value["sessions"]) == 100 and all(s["name"].endswith("é🎵") for s in value["sessions"])
        complete += 1
    assert complete and {"audio-system.state", "audio-system.snapshot.result", "audio-system.command.result",
                         "audio-system.inventory.result", "audio-system.read.error"} <= kinds
    print(json.dumps({"status": "passed", "nativeEnvelopes": len(envelopes), "maximumFullFrameBytes": maximum,
                      "reassembledInventories": complete, "schemaKinds": sorted(kinds)}))


if __name__ == "__main__":
    main(sys.argv[1])
