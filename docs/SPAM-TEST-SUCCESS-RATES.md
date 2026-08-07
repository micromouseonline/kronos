# Beacon-Spam Stress Test: Run Success Rates by Condition

Date: 2026-08-06

Computed directly from `test-data/spam-tests/*.log` (cerberus's own `<4,4>`
RUNNING / `<4,5>` committed display-protocol markers — the same convention
used elsewhere in `NETWORK-TIMING-LOG.md`), not copied from that doc's
prose. See that document for full session narratives; this is a
run-count/success-rate rollup by spammer condition only.

**Definitions:** "started" = cerberus logged the run entering `RUNNING`
(armed+started received). "completed successfully" = cerberus logged a
committed result for that run (a GOAL that wasn't rejected as stale).

## No spammer (uncontested airspace)

| Session | Started | Completed | Rate |
|---|---|---|---|
| Session 6 (5000-run baseline) | 5000 | 5000 | 100.00% |
| Early mini-trials, runs 1-15 of 3 sessions (`-3`/`-4`/`-5` files) | 45 | 44 | 97.8% |
| **Total** | **5045** | **5044** | **99.98%** |

One loss, in the very first mini-trial's run 1 (documented cold-start
artifact, not a real fault).

## Busy (single beacon spammer)

| Session | Started | Completed | Rate |
|---|---|---|---|
| Early mini-trials, runs 16-30 of 3 sessions | 45 | 45 | 100.00% |
| Session 7 (5000-run trial, weak RSSI, cut short by a since-fixed crash bug) | 2882 | 2881 | 99.97% |
| Session 8 (5000-run trial, AP repositioned) | 5345 | 5344 | 99.98% |
| Session 9 (short marginal trial, spammer+BT, AP moved closer — busier than a plain single spammer) | 399 | 398 | 99.75% |
| Session 11 (single-spammer, `MIN_MODEM`) | 4990 | 4985 | 99.90% |
| Session 12 (single-spammer, `MIN_MODEM` repeat) | 9999 | 9998 | 99.99% |
| **Total** | **23660** | **23651** | **99.96%** |

## Adversarial (two simultaneous spammers, most also with BT streaming)

| Session | Started | Completed | Rate |
|---|---|---|---|
| Early mini-trials, post-2nd-spammer phase of 3 sessions | 43 | 41 | 95.3% |
| Session 10 (short marginal trial, two spammers+BT) | 399 | 399 | 100.00% |
| Session 13 (`MIN_MODEM`, two-spammer+BT smoke test) | 4993 | 4987 | 99.88% |
| Session 14 (`MIN_MODEM`, two-spammer+BT, reverted heartbeat-widening experiment) | 2950 | 2607 | 88.37% |
| Session 15 (`NONE`, two-spammer+BT, compromised: one spammer dropped out ~30min in) | 4992 | 4986 | 99.88% |
| Session 15a (`NONE`, two-spammer+BT, compromised: BT found off partway) | 2984 | 2947 | 98.76% |
| **Total** | **16361** | **15967** | **97.59%** |
| **Total excluding session 14** (reverted, non-shipped firmware variant) | **13411** | **13360** | **99.62%** |

Session 14 alone accounts for most of the adversarial category's loss —
that trial ran a heartbeat-tolerance change that was tested, found to make
things dramatically worse, and reverted same-day; it's not representative
of the shipped firmware. With it excluded, the adversarial pass rate is
close to the busy category's.

## Caveats

- The earliest ("pre", unnumbered) session is excluded entirely — it
  predates the retry/deadline fixes and ran different (buggy) firmware, so
  it isn't comparable to the rest.
- The 15/15/rest split inside the three early mini-trials is inferred from
  the documented protocol (15 no-spam, 15 single-spam, then 2nd spammer)
  since retries only start once the 2nd spammer engages — there's no log
  marker distinguishing runs 1-15 from 16-30 directly.
- Session 9's spammer count is stated ambiguously in
  `NETWORK-TIMING-LOG.md` ("spammer(s)"); session 10's text implies
  session 9 was the lighter of the two, so it's bucketed here as busy, not
  adversarial.
