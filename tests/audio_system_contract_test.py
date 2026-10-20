"""Offline conformance vectors/model; never imports or starts a native runtime."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest

from jsonschema import Draft202012Validator, ValidationError, validators

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contracts/audio-system/v1"
SCHEMA = json.loads((CONTRACT / "schema.json").read_text())
EXAMPLES = json.loads((CONTRACT / "examples.json").read_text())
LIMITS = SCHEMA["x-service"]["limits"]


def utf8_bytes(validator, maximum, instance, schema):
    if isinstance(instance, str) and len(instance.encode("utf-8")) > maximum:
        yield ValidationError("ID exceeds UTF-8 byte bound")


Validator = validators.extend(Draft202012Validator, {"x-maxUtf8Bytes": utf8_bytes})
VALIDATOR = Validator(SCHEMA)


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def validate(value, definition=None):
    serialized = compact(value)  # Also reject non-JSON NaN/infinity.
    if definition:
        Validator({"$ref": "#/$defs/" + definition, "$defs": SCHEMA["$defs"]}).validate(value)
    else:
        VALIDATOR.validate(value)
        if len(serialized) > LIMITS["jsonBytes"]:
            raise ValueError("message_too_large")


def unique(items):
    ids = [item["id"] for item in items]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate local ID")


def validate_state(state, bounded=True):
    validate(state, None if bounded else state["topic"] + "State")
    if state["body"]["mode"] != "inline" or state["body"]["value"] is None:
        return
    value = state["body"]["value"]
    unique(value["sessions"])
    if state["topic"] == "audio":
        unique(value["endpoints"])
        endpoints = {item["id"]: item for item in value["endpoints"]}
        for session in value["sessions"]:
            endpoint = endpoints.get(session["endpointId"])
            if not endpoint:
                raise ValueError("orphaned session")
            if endpoint["capture"] and any(session["capabilities"].values()):
                raise ValueError("capture session cannot be writable")
        empty = not value["endpoints"] and not value["sessions"]
        if value["complete"] == bool(value["errors"]):
            raise ValueError("incomplete enumeration must remain observable")
        if empty and not value["complete"]:
            raise ValueError("failed enumeration cannot prove empty")
    else:
        selected = value["selected"]
        empty = selected is None
        if selected:
            session = next((s for s in value["sessions"] if s["id"] == selected["id"]), None)
            if not session or session["capabilities"] != selected["capabilities"]:
                raise ValueError("selected session capabilities mismatch")
            if selected["seekStartMs"] > selected["seekEndMs"]:
                raise ValueError("invalid seek range")
    if (state["availability"] == "empty") != empty:
        raise ValueError("empty and ready are distinct")


def validate_snapshot(result):
    validate(result)
    for topic in ("audio", "media"):
        validate_state(result[topic])
        if result[topic]["generation"] != result["generation"]:
            raise ValueError("mixed snapshot generations")


def validate_result(request, result, *, dispatched):
    validate(result)
    if (request["requestId"], request["generation"]) != (result["requestId"], result["generation"]):
        raise ValueError("uncorrelated or late result")
    if dispatched and not result["ok"] and result["outcome"] != "unknown":
        raise ValueError("native failure cannot prove no effect")
    if not dispatched and result["outcome"] != "rejected":
        raise ValueError("no dispatch cannot be acknowledged or uncertain")


def observe(previous, event):
    validate_state(event["value"])
    if previous is None:
        if event["kind"] != "snapshot":
            raise ValueError("missing initial snapshot")
    elif (event["kind"] != "state" or event["sequence"] <= previous["sequence"]
          or event["value"]["topic"] != previous["value"]["topic"]
          or event["value"]["generation"] != previous["value"]["generation"]
          or event["value"]["revision"] != previous["value"]["revision"] + 1):
        raise ValueError("resynchronize before command")
    return event


def predispatch(request, state, *, enabled=True, microphone=False):
    """Contract fixture oracle, not a provider or an authorization mechanism."""
    validate(request, "commandRequest")
    validate_state(state)
    if request["generation"] != state["generation"]:
        return "stale_generation"
    if request["revision"] != state["revision"]:
        return "stale_revision"
    command = request["command"]
    topic = command["kind"].split(".")[0]
    if state["topic"] != topic:
        return "invalid_request"
    if not enabled:
        return "permission_denied"
    if state["availability"] in ("absent", "unavailable"):
        return "unavailable"
    value = state["body"]["value"]
    action = command["action"]
    if action in ("refresh", "show"):
        return None
    if topic == "audio":
        group = value["endpoints"] if action.startswith("endpoint") else value["sessions"]
        target = next((item for item in group if item["id"] == command["id"]), None)
        if not target:
            return "stale_session"
        capture = target.get("capture", next((e["capture"] for e in value["endpoints"] if e["id"] == target.get("endpointId")), False))
        if capture and (not action.startswith("endpoint") or not microphone):
            return "permission_denied"
        if not target["capabilities"]["volume" if action.endswith("Volume") else "mute"]:
            return "unsupported"
    elif action == "session":
        if not any(s["id"] == command["value"] for s in value["sessions"]):
            return "stale_session"
    else:
        selected = value["selected"]
        if not selected or selected["id"] != request["expectedSessionId"]:
            return "stale_session"
        if not selected["capabilities"][action]:
            return "unsupported"
        if action == "seek" and not selected["seekStartMs"] <= command["value"] <= selected["seekEndMs"]:
            return "invalid_request"
    return None


def paged(state):
    value = state["body"]["value"]
    raw = compact(value)
    if len(raw) > LIMITS["providerInventoryBytesTotal"]:
        raise ValueError("message_too_large")
    projected = copy.deepcopy(state)
    projected["body"] = {"mode": "paged", "inventoryId": "synthetic-inventory", "totalBytes": len(raw), "sha256": "sha256:" + hashlib.sha256(raw).hexdigest()}
    validate(projected)
    return projected, raw


def page(state, raw, offset):
    if offset % LIMITS["inventoryChunkBytes"] or not 0 <= offset < len(raw):
        raise ValueError("invalid offset")
    chunk = raw[offset:offset + LIMITS["inventoryChunkBytes"]]
    next_offset = offset + len(chunk)
    result = {"kind": "audio-system.inventory.result", "requestId": "page-" + str(offset), "generation": state["generation"], "topic": state["topic"], "revision": state["revision"], "inventoryId": state["body"]["inventoryId"], "offset": offset, "totalBytes": len(raw), "sha256": state["body"]["sha256"], "encoding": "base64-utf8-json", "data": base64.b64encode(chunk).decode("ascii"), "nextOffset": next_offset if next_offset < len(raw) else None}
    validate(result)
    return result


def assemble(state, pages, *, elapsed_ms=0, revoked=False):
    if revoked or elapsed_ms >= LIMITS["inventoryReadDeadlineMs"]:
        raise ValueError("revoked or deadline")
    if len(pages) > 512:
        raise ValueError("too many pages")
    raw = bytearray()
    for index, part in enumerate(pages):
        validate(part, "inventoryResult")
        for key in ("generation", "topic", "revision"):
            if part[key] != state[key]:
                raise ValueError("stale inventory")
        for key in ("inventoryId", "totalBytes", "sha256"):
            if part[key] != state["body"][key]:
                raise ValueError("inventory mismatch")
        if part["offset"] != len(raw):
            raise ValueError("out of order or duplicated page")
        chunk = base64.b64decode(part["data"], validate=True)
        size = LIMITS["inventoryChunkBytes"]
        expected = min(size, part["totalBytes"] - len(raw))
        if len(chunk) != expected or not chunk:
            raise ValueError("short or oversized page")
        raw.extend(chunk)
        if len(raw) > LIMITS["providerInventoryBytesTotal"]:
            raise ValueError("inventory budget")
        expected_next = len(raw) if len(raw) < part["totalBytes"] else None
        if part["nextOffset"] != expected_next:
            raise ValueError("invalid continuation")
        if expected_next is None and index != len(pages) - 1:
            raise ValueError("extra page")
    if len(raw) != state["body"]["totalBytes"] or "sha256:" + hashlib.sha256(raw).hexdigest() != state["body"]["sha256"]:
        raise ValueError("incomplete inventory or checksum mismatch")
    value = json.loads(raw.decode("utf-8"))
    validate(value, state["topic"] + "Value")
    # Apply semantic state invariants without imposing the inline message limit.
    restored = copy.deepcopy(state)
    restored["body"] = {"mode": "inline", "value": value}
    validate_state(restored, bounded=False)
    return value


class ContractTests(unittest.TestCase):
    def fixture(self, name):
        return copy.deepcopy(EXAMPLES[name])

    def test_public_schema_identity_and_examples(self):
        Draft202012Validator.check_schema(SCHEMA)
        self.assertEqual((SCHEMA["x-service"]["contract"], SCHEMA["x-service"]["version"]), ("a18/audio-system", 1))
        for value in EXAMPLES.values():
            validate(value)
        for name in ("audioState", "mediaState"):
            validate_state(EXAMPLES[name])
        validate_snapshot(EXAMPLES["snapshotResult"])
        snapshot = self.fixture("snapshotResult")
        snapshot["media"]["generation"] = "different-provider-generation"
        with self.assertRaises(ValueError):
            validate_snapshot(snapshot)
        # No abandoned schema fragment behind an unreferenced export.
        visited = set()
        def walk(value):
            if isinstance(value, dict):
                if "$ref" in value:
                    name = value["$ref"].removeprefix("#/$defs/")
                    if name not in visited:
                        visited.add(name)
                        walk(SCHEMA["$defs"][name])
                for key, child in value.items():
                    if key != "$defs":
                        walk(child)
            elif isinstance(value, list):
                for child in value:
                    walk(child)
        walk(SCHEMA)
        self.assertEqual(visited, set(SCHEMA["$defs"]))

    def test_all_existing_commands_and_no_new_system_setter(self):
        audio = SCHEMA["$defs"]["audioCommand"]["oneOf"]
        media = SCHEMA["$defs"]["mediaCommand"]["oneOf"]
        self.assertEqual({x["properties"]["action"]["const"] for x in audio}, {"refresh", "show", "endpointVolume", "sessionVolume", "endpointMute", "sessionMute"})
        self.assertEqual({x["properties"]["action"]["const"] for x in media}, {"refresh", "session", "playPause", "previous", "next", "seek", "shuffle", "repeat"})
        # Exercise every shape, not just enum equality.
        for topic, variants in (("audio", audio), ("media", media)):
            for variant in variants:
                command = {"kind": topic + ".command", "action": variant["properties"]["action"]["const"]}
                for field in set(variant["required"]) - {"kind", "action"}:
                    values = {"id": "owned", "volume": 25, "muted": False}
                    if field == "value":
                        command[field] = {"session": "owned", "seek": 100, "shuffle": True, "repeat": "none"}[command["action"]]
                    else:
                        command[field] = values[field]
                validate(command, topic + "Command")
        request = self.fixture("commandRequest")
        for action in ("defaultDevice", "route", "playSound", "setMicrophonePermission"):
            request["command"]["action"] = action
            with self.assertRaises(ValidationError):
                validate(request)

    def test_units_shapes_and_utf8_bounds(self):
        request = self.fixture("commandRequest")
        for bad in (-1, 101, "50", True):
            request["command"]["volume"] = bad
            with self.assertRaises(ValidationError):
                validate(request)
        request = self.fixture("commandRequest")
        request["command"]["id"] = "é" * 2048
        validate(request)
        request["command"]["id"] += "é"
        with self.assertRaises(ValidationError):
            validate(request)
        for field, bad in (("timeoutMs", 30001), ("requestId", ""), ("generation", "")):
            request = self.fixture("commandRequest")
            request[field] = bad
            with self.assertRaises(ValidationError):
                validate(request)

    def test_absent_empty_unavailable_are_not_interchangeable(self):
        state = self.fixture("mediaState")
        for availability in ("absent", "unavailable"):
            state["availability"] = availability
            state["body"]["value"] = None
            state["error"] = None if availability == "absent" else {"code": "unavailable", "message": "Synthetic failure"}
            validate_state(state)
        state["error"] = None
        with self.assertRaises(ValidationError):
            validate_state(state)
        state = self.fixture("mediaState")
        state["availability"] = "empty"
        state["body"]["value"]["selected"] = None
        validate_state(state)  # Other unselected sessions may still exist.
        state["availability"] = "ready"
        with self.assertRaises(ValueError):
            validate_state(state)

    def test_capabilities_permissions_and_local_ids(self):
        state = self.fixture("audioState")
        request = self.fixture("commandRequest")
        self.assertIsNone(predispatch(request, state))
        self.assertEqual(predispatch(request, state, enabled=False), "permission_denied")
        request["command"] = {"kind": "audio.command", "action": "endpointMute", "id": "capture-owned", "muted": True}
        self.assertEqual(predispatch(request, state), "permission_denied")
        self.assertEqual(predispatch(request, state, microphone=True), "unsupported")
        request["command"]["id"] = "foreign-provider-id"
        self.assertEqual(predispatch(request, state), "stale_session")
        state["body"]["value"]["sessions"][0]["endpointId"] = "missing"
        with self.assertRaises(ValueError):
            validate_state(state)
        state["body"]["value"]["sessions"][0]["endpointId"] = "capture-owned"
        with self.assertRaises(ValueError):
            validate_state(state)
        state = self.fixture("audioState")
        state["body"]["value"]["sessions"].append(copy.deepcopy(state["body"]["value"]["sessions"][0]))
        with self.assertRaises(ValueError):
            validate_state(state)

    def test_initial_snapshot_order_and_generation_replacement(self):
        first = {"kind": "snapshot", "sequence": 13, "value": self.fixture("audioState")}
        observed = observe(None, first)
        next_event = copy.deepcopy(first)
        next_event.update(kind="state", sequence=14)
        next_event["value"]["revision"] += 1
        observe(observed, next_event)
        for field, bad in (("revision", 10), ("generation", "new-generation")):
            changed = copy.deepcopy(next_event)
            changed["value"][field] = bad
            with self.assertRaises(ValueError):
                observe(observed, changed)
        with self.assertRaises(ValueError):
            observe(None, next_event)
        with self.assertRaises(ValueError):
            observe(next_event, first)

    def test_media_target_pin_capability_and_seek(self):
        state = self.fixture("mediaState")
        request = self.fixture("commandRequest")
        request.update(revision=state["revision"], expectedSessionId="media-owned", command={"kind": "media.command", "action": "playPause"})
        self.assertIsNone(predispatch(request, state))
        request["expectedSessionId"] = "disappeared"
        self.assertEqual(predispatch(request, state), "stale_session")
        request["expectedSessionId"] = "media-owned"
        request["command"]["action"] = "next"
        self.assertEqual(predispatch(request, state), "unsupported")
        request["command"] = {"kind": "media.command", "action": "seek", "value": 3001}
        self.assertEqual(predispatch(request, state), "invalid_request")
        request["command"]["value"] = 3000
        self.assertIsNone(predispatch(request, state))
        state["body"]["value"]["sessions"][0]["capabilities"] = None
        with self.assertRaises(ValueError):
            validate_state(state)

    def test_request_correlation_generation_revocation_and_no_replay(self):
        request = self.fixture("commandRequest")
        state = self.fixture("audioState")
        sent = []
        seen = set()
        def dispatch(connection, command):
            key = (connection, command["generation"], command["requestId"])
            if key in seen:
                return "invalid_request"
            seen.add(key)
            error = predispatch(command, state)
            if error:
                return error
            sent.append(key)
            return None
        self.assertIsNone(dispatch("connection-a", request))
        self.assertEqual(dispatch("connection-a", request), "invalid_request")
        self.assertIsNone(dispatch("connection-b", request))
        result = self.fixture("unknownResult")
        validate_result(request, result, dispatched=True)
        changed = copy.deepcopy(result)
        changed["requestId"] = "other-consumer-id"
        with self.assertRaises(ValueError):
            validate_result(request, changed, dispatched=True)
        # A timeout cannot turn an in-flight command into a certain rejection.
        for reason in ("timeout", "cancelled", "revoked", "native_failure"):
            result["error"]["code"] = reason
            validate_result(request, result, dispatched=True)
            changed = copy.deepcopy(result)
            changed["outcome"] = "rejected"
            with self.assertRaises(ValueError):
                validate_result(request, changed, dispatched=True)
            validate_result(request, changed, dispatched=False)
        state["generation"] = "synthetic-generation-2"
        request["requestId"] = "new-request"
        self.assertEqual(dispatch("connection-a", request), "stale_generation")
        request["generation"] = state["generation"]
        request["revision"] -= 1
        request["requestId"] = "other-request"
        self.assertEqual(dispatch("connection-a", request), "stale_revision")
        self.assertEqual(len(sent), 2)  # Neither timeout nor reconnect replayed it.
        result["ok"] = True
        with self.assertRaises(ValidationError):
            validate(result)

    def oversized(self):
        state = self.fixture("audioState")
        template = state["body"]["value"]["sessions"][0]
        state["body"]["value"]["sessions"] = [{**template, "id": "owned-" + str(i), "name": "é🎵" * 150} for i in range(100)]
        self.assertGreater(len(compact(state)), LIMITS["jsonBytes"])
        return paged(state)

    def test_large_inventory_lossless_unicode_and_packet_budget(self):
        state, raw = self.oversized()
        with self.assertRaises(ValueError):
            validate({**EXAMPLES["audioState"], "body": {"mode": "inline", "value": json.loads(raw)}})
        pages = [page(state, raw, offset) for offset in range(0, len(raw), LIMITS["inventoryChunkBytes"])]
        self.assertTrue(all(len(compact(p)) <= 65536 for p in pages))
        value = assemble(state, pages)
        self.assertEqual(len(value["sessions"]), 100)
        self.assertEqual(compact(value), raw)
        # Even a single metadata string larger than a message is never truncated.
        media = self.fixture("mediaState")
        media["body"]["value"]["selected"]["title"] = "é🎵" * 20000
        projected, serialized = paged(media)
        chunks = [page(projected, serialized, offset) for offset in range(0, len(serialized), 32768)]
        self.assertEqual(assemble(projected, chunks)["selected"]["title"], media["body"]["value"]["selected"]["title"])

    def test_continuation_rejects_stale_reordered_corrupt_or_expired(self):
        state, raw = self.oversized()
        pages = [page(state, raw, offset) for offset in range(0, len(raw), 32768)]
        bad_cases = [pages[::-1], pages[:-1], [pages[0], pages[0], *pages[1:]]]
        for field, bad in (("generation", "old-generation"), ("revision", 0), ("inventoryId", "foreign-inventory"), ("nextOffset", 1), ("sha256", "sha256:" + "0" * 64), ("data", "AAAA")):
            changed = copy.deepcopy(pages)
            changed[0][field] = bad
            bad_cases.append(changed)
        corrupted = copy.deepcopy(pages)
        data = base64.b64decode(corrupted[0]["data"])
        corrupted[0]["data"] = base64.b64encode(bytes([data[0] ^ 1]) + data[1:]).decode("ascii")
        bad_cases.append(corrupted)
        for changed in bad_cases:
            with self.assertRaises((ValueError, ValidationError)):
                assemble(state, changed)
        with self.assertRaises(ValueError):
            assemble(state, pages, elapsed_ms=5000)
        with self.assertRaises(ValueError):
            assemble(state, pages, revoked=True)
        changed = copy.deepcopy(state)
        changed["revision"] += 1
        with self.assertRaises(ValueError):
            assemble(changed, pages)

    def test_total_budget_and_observable_refusal(self):
        state = self.fixture("audioState")
        state["body"]["value"]["runtime"]["notice"] = "x" * (LIMITS["providerInventoryBytesTotal"] + 1)
        with self.assertRaises(ValueError):
            paged(state)
        error = self.fixture("audioState")
        error.update(availability="unavailable", error={"code": "message_too_large", "message": "Synthetic total inventory refusal"}, body={"mode": "inline", "value": None})
        validate_state(error)
        self.assertEqual(LIMITS["retainedBytesPerBrokerSession"], 16 * 1024 * 1024)
        self.assertEqual(LIMITS["queuedBytesPerBrokerSession"], 16 * 1024 * 1024)
        self.assertEqual(LIMITS["providerInventoryBytesTotal"] // LIMITS["inventoryChunkBytes"], 512)

    def test_combined_snapshot_switches_to_reference_without_truncation(self):
        result = self.fixture("snapshotResult")
        for topic in ("audio", "media"):
            if topic == "audio":
                result[topic]["body"]["value"]["runtime"]["notice"] = "é" * 18000
            else:
                result[topic]["body"]["value"]["selected"]["title"] = "é" * 18000
            validate_state(result[topic])
        with self.assertRaises(ValueError):
            validate_snapshot(result)
        largest = max(("audio", "media"), key=lambda topic: len(compact(result[topic])))
        before = compact(result[largest]["body"]["value"])
        result[largest], raw = paged(result[largest])
        self.assertEqual(raw, before)
        validate_snapshot(result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
