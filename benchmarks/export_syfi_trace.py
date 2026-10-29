"""Export selected SYFI sessions as raw records and synthetic replay traces.

No original prompts are present in SYFI. Prefix reuse here is a conservative
512-token block approximation limited to the preceding INPUT, not its output.
Timestamps are synthetic arrivals, not completion dependencies. The current
replayer does not enforce session-serial execution.
"""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path


SELECTIONS = {
    "syfi_coding_small_64k": [
        ("codex:a28fd512-38a1-5bca-7d56-f765c8f67bb4", 0, 111),
        ("codex:46ca40e9-c5df-c385-86b4-3e487705bea1", 4, 67),
        ("codex:1104404c-78c6-7b62-23d0-4fe77ca8182f", 7, 38),
        ("claude:f8252255-6f05-c9e2-561b-2b99f89bf9e4", 17, 80),
    ],
    "syfi_coding_compact_128k": [
        ("codex:9d118163-ffda-ce06-323f-444b74987dc5", 6, 133),
    ],
    "syfi_coding_complete_256k": [
        ("claude:5fe601f0-e6ed-33bc-f475-f787e7fde9a8", 0, 567),
    ],
}


def export(source, output, interval_ms, selections=None, selection_manifest=None):
    selections = SELECTIONS if selections is None else selections
    selected = defaultdict(list)
    wanted = {session for group in selections.values() for session, _, _ in group}
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        for line_number, line in enumerate(stream, 1):
            digest.update(line)
            row = json.loads(line)
            if row.get("session_id") in wanted:
                selected[row["session_id"]].append((line_number, row))
    if selection_manifest and digest.hexdigest() != selection_manifest["source_sha256"]:
        raise ValueError("source SHA256 differs from the analyzed dataset")
    report = {
        "source": str(source), "source_sha256": digest.hexdigest(),
        "block_tokens": 512, "arrival_interval_ms": interval_ms,
        "warnings": [
            "Synthetic content, not original code or DeepSeek-retokenized prompts.",
            "Only complete 512-token prefixes within the preceding input are reused.",
            "Session/model/reset boundaries have distinct synthetic block identities.",
            "Synthetic arrivals do not enforce predecessor completion; hits are not guaranteed.",
            "Client length filtering can remove predecessors and reduce reuse.",
        ],
        "exports": {},
    }
    next_id = 1000000
    for name, ranges in selections.items():
        sessions = []
        raw_rows = []
        for session, first, last in ranges:
            rows = sorted(
                [(line, row) for line, row in selected[session]
                 if first <= row["round_index"] <= last],
                key=lambda pair: pair[1]["round_index"],
            )
            if [r["round_index"] for _, r in rows] != list(range(first, last + 1)):
                raise ValueError(f"missing or duplicate rounds in {session}")
            if len({(r.get("provider"), r.get("project")) for _, r in rows}) != 1:
                raise ValueError(f"ambiguous session identity: {session}")
            previous = None
            blocks = []
            replay = []
            for line, row in rows:
                length = row["input_tokens_total"]
                if length <= 0 or row["output_tokens"] <= 0:
                    raise ValueError(f"nonpositive lengths at source line {line}")
                shared = 0
                if (previous and previous["model"] == row["model"]
                        and length >= previous["input_tokens_total"]):
                    shared = min(row["prefix_tokens"], previous["input_tokens_total"], length) // 512
                shared = min(shared, len(blocks))
                retained = blocks[:shared]
                count = (length + 511) // 512
                fresh = count - shared
                blocks = retained + list(range(next_id, next_id + fresh))
                next_id += fresh
                replay.append({
                    "input_length": length, "output_length": row["output_tokens"],
                    "hash_ids": blocks.copy(), "session_id": session,
                    "round_index": row["round_index"], "source_line": line,
                    "source_model": row["model"], "source_prefix_tokens": row["prefix_tokens"],
                    "synthetic_reused_input_tokens": shared * 512,
                })
                raw_rows.append(row)
                previous = row
            sessions.append(replay)
        # Round-robin interleaves independent sessions without changing their order.
        replay_rows = [rows[index] for index in range(max(map(len, sessions)))
                       for rows in sessions if index < len(rows)]
        for index, row in enumerate(replay_rows):
            row["timestamp"] = index * interval_ms
        for suffix, rows in [("raw", raw_rows), ("replay", replay_rows)]:
            path = output / f"{name}_{suffix}.jsonl"
            with path.open("w") as stream:
                for row in rows:
                    stream.write(json.dumps(row, ensure_ascii=False) + "\n")
        inputs = sum(r["input_length"] for r in replay_rows)
        unique_blocks = set()
        for row in replay_rows:
            chain = b""
            for block in row["hash_ids"][:row["input_length"] // 512]:
                chain = hashlib.sha256(chain + str(block).encode() + b"\0").digest()
                unique_blocks.add(chain)
        expected = selection_manifest["exports"][name] if selection_manifest else None
        if expected and (len(replay_rows) != expected["requests"]
                         or len(unique_blocks) != expected["unique_blocks"]
                         or inputs != expected["input_tokens"]):
            raise ValueError(f"export differs from analyzed selection: {name}")
        report["exports"][name] = {
            "selection": [dict(session_id=s, first_round=a, last_round=b) for s, a, b in ranges],
            "requests": len(replay_rows), "input_tokens": inputs,
            "output_tokens": sum(r["output_length"] for r in replay_rows),
            "max_input_tokens": max(r["input_length"] for r in replay_rows),
            "max_total_tokens": max(r["input_length"] + r["output_length"] for r in replay_rows),
            "synthetic_reused_input_fraction": sum(r["synthetic_reused_input_tokens"] for r in replay_rows) / inputs,
            "arrival_span_ms": replay_rows[-1]["timestamp"],
            "unique_full_512_chained_blocks": len(unique_blocks),
            "estimated_fa_bytes": len(unique_blocks) * 3186688,
            "estimated_wa_bytes": len(unique_blocks) * 6496256,
            "raw_file": str(output / f"{name}_raw.jsonl"),
            "replay_file": str(output / f"{name}_replay.jsonl"),
        }
    manifest_name = "syfi_large_export_manifest.json" if selection_manifest else "syfi_export_manifest.json"
    with (output / manifest_name).open("w") as stream:
        json.dump(report, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path.home() / "downloads/traces/syfi_coding_trace.jsonl")
    parser.add_argument("--output-dir", type=Path, default=Path.home() / "downloads/traces")
    parser.add_argument("--interval-ms", type=int, default=1000)
    parser.add_argument("--selection-manifest", type=Path,
                        help="analysis manifest containing the two large candidate profiles")
    args = parser.parse_args()
    if not args.output_dir.is_dir() or args.interval_ms < 1:
        parser.error("output directory must exist and interval must be positive")
    selections = manifest = None
    if args.selection_manifest:
        analysis = json.loads(args.selection_manifest.read_text())
        profiles = {
            "syfi_coding_large_64k": "65536_0.9_16_230GiB",
            "syfi_coding_large_128k": "131072_0.9_16_230GiB",
        }
        manifest = {"source_sha256": analysis["source_sha256"], "exports": {}}
        selections = {}
        for name, key in profiles.items():
            profile = analysis["profiles"][key]
            manifest["exports"][name] = profile
            selections[name] = [(r["session_id"], r["first_round"], r["last_round"])
                                for r in profile["selection"]]
        # Preserve the exact analyzed source identities/line ranges next to exports.
        with (args.output_dir / "syfi_large_selection_manifest.json").open("w") as stream:
            json.dump(manifest, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    export(args.source, args.output_dir, args.interval_ms, selections, manifest)
