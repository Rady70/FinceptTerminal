"""Build deterministic Batch C UI evidence through the real Batch A/B backend.

Captured provider fixtures use FakeTransport; history is a temporary real SQLite
database. This helper never contacts live providers. Qt tests subsequently read
the database with the shipped fedwatch_data.py CLI, not canned history JSON.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "marketlab" / "tests"))

from fedwatch_test_support import FakeTransport, FixedClock, epoch, make_clob_history, make_snapshot_transport, utc
from test_fedwatch_analytics import seed
from fedwatch import history, snapshot
from fedwatch.store import FedwatchHistoryStore
from fedwatch.transport import TransportError


def backfill_fixture(store, case, meeting="2026-10-28"):
    now = utc(2026, 9, 28, 12)
    transport = FakeTransport()
    if case == "failure":
        transport.add_json("prices-history", TransportError("fixture HTTP 500", status_code=500))
    else:
        outcome = -25 if case.startswith("coverage-") else 25
        mapping = next(m for m in store.validated_mappings([meeting])
                       if m["outcome_bp"] == outcome and not m["open_ended"])
        points = [{"t": epoch(utc(2026, 8, 2)), "p": 0.42}]
        if case.startswith("coverage-"):
            points = [{"t": epoch(utc(2026, 8, day + 1)), "p": 0.003 + day * 0.0001}
                      for day in range(8 if case == "coverage-normal" else 1)]
        if case == "partial":
            points.extend([{"t": "invalid", "p": 0.3},
                           {"t": epoch(utc(2026, 8, 3)), "p": 2.5},
                           {"t": epoch(utc(2030, 1, 1)), "p": 0.5}])
        transport.add_json("prices-history", make_clob_history({mapping["external_token_id"]: points}))
    return history.backfill_polymarket(store, transport, meeting_dates=[meeting], force=True,
                                      clock=FixedClock(now), sleep=lambda _: None)


def main() -> None:
    store = FedwatchHistoryStore(Path(sys.argv[1]))
    case = sys.argv[2] if len(sys.argv) > 2 else ""
    if case.startswith("backfill-"):
        meeting = sys.argv[3] if len(sys.argv) > 3 else "2026-10-28"
        print(json.dumps({"success": True, "data": backfill_fixture(store, case.removeprefix("backfill-"), meeting)},
                         allow_nan=False))
        return
    if case == "mapping-without-history":
        outcome = int(sys.argv[3]) if len(sys.argv) > 3 else 25
        mapping = next(m for m in store.validated_mappings(["2026-10-28"])
                       if m["outcome_bp"] == outcome and not m["open_ended"])
        store.upsert_mapping_outcome(
            mapping["meeting_date"], mapping["source"], mapping["method"], outcome, False, "VALIDATED",
            mapping["external_event_id"], mapping["external_event_title"], mapping["external_market_id"],
            "fixture-new-token-without-observations", mapping["question"], {"fixture": True}, now=utc(2026, 9, 28, 12))
        print(json.dumps({"success": True}))
        return
    now = utc(2026, 9, 28, 12)
    envelope = snapshot.build_snapshot(
        make_snapshot_transport(now), clock=FixedClock(now), sleep=lambda _: None
    )
    history.record_snapshot(store, envelope["data"], clock=FixedClock(now))
    seed(store, [("2026-08-01T12:00:00Z", 30), ("2026-09-26T12:00:00Z", 35)], outcome_bp=25)
    mappings = [m for m in store.validated_mappings() if m["meeting_date"] == "2026-10-28"]
    mapping = next(m for m in mappings if m["outcome_bp"] == 25 and not m["open_ended"])
    seed(store, [("2026-08-01T12:00:00Z", 20), ("2026-09-27T12:00:00Z", 32)],
         method=history.POLY_METHOD, source=history.POLY_SOURCE,
         instrument_key=mapping["external_token_id"])
    seed(store, [("2026-08-01T00:00:00Z", 45)], method=history.FED_METHOD_ZQ, source=history.ZQ_SOURCE)

    # Resolved history has no live collection requirement and its actual outcome
    # is supplied explicitly by the store, never inferred by the UI.
    resolved = "2026-09-16"
    store.upsert_meeting(resolved, now=now)
    seed(store, [("2026-09-01T12:00:00Z", 60), ("2026-09-13T12:00:00Z", 65),
                 ("2026-09-14T12:00:00Z", 70), ("2026-09-15T12:00:00Z", 75)], meeting=resolved)
    seed(store, [("2026-09-01T12:00:00Z", 55), ("2026-09-13T12:00:00Z", 60),
                 ("2026-09-14T12:00:00Z", 65), ("2026-09-15T12:00:00Z", 70)],
         meeting=resolved, method=history.POLY_METHOD, source=history.POLY_SOURCE)
    store.mark_resolved(resolved, 25, "deterministic UI fixture", now=now)
    if case == "resolved-alternative-outcomes":
        for outcome in [-25, 0]:
            seed(store, [("2026-09-14T12:00:00Z", 10)], meeting=resolved, outcome_bp=outcome)
    if case == "review":
        backfill_fixture(store, "partial")
        backfill_fixture(store, "failure", "2026-12-09")
    print(json.dumps(envelope, allow_nan=False))


if __name__ == "__main__":
    main()
