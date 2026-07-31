// ----------------------------------------------------------------------------
//  gate-event-dedup.h — Small fixed-size de-duplication cache for gate events
//  (NETWORK-TIMING-ISSUE.md recommendation 6), bundled alongside hesperus's
//  retry mechanism (recommendation 1's follow-on, status section item 1):
//  once hesperus can resend an event after a lost ack, cerberus may see the
//  same (gate_id, event, tsf_us) twice -- this recognises the repeat so it
//  isn't re-dispatched into the race state machine a second time, while
//  still being reported as "handled" so the caller acks it (the whole point
//  of a retry is to guarantee eventual acknowledgement, and cerberus
//  genuinely already has this event).
//
//  Keyed on (gate_id, event), not tsf_us alone -- tsf_us is a shared Wi-Fi
//  TSF clock across every station on the same AP (NETWORK-TIMING-ISSUE.md
//  #3), so two different boards' tsf_us values live in the same numeric
//  space and could plausibly collide; gate_id+event avoids a false-positive
//  cross-board match.
// ----------------------------------------------------------------------------
#pragma once

#include <Arduino.h>
#include <cstring>

struct DedupEntry {
  char gate_id[32] = "";
  char event[16] = "";
  uint64_t last_tsf_us = 0;
};

// ~2-3 concurrently-connected boards x 3 events, with margin for boards.h's
// catalogued gate_id fleet being swapped in/out over a long cerberus uptime
// without a reboot -- see system-event-queue.h's xSystemEventQueue (32
// slots) for a similar "small fixed size is fine at this scale" precedent.
constexpr size_t DEDUP_TABLE_SIZE = 16;

inline DedupEntry g_dedup_table[DEDUP_TABLE_SIZE];
inline size_t g_dedup_next_slot = 0;  // round-robin overwrite once every slot is in use

/// @brief Returns true if (gate_id, event, tsf_us) was already seen -- the
/// caller must still treat this as handled (ack it) without re-dispatching,
/// since cerberus genuinely already has this event; the most likely reason
/// it's arriving again is that the *ack*, not the original event, was lost.
/// No reset on RESTART/NEW_MOUSE is needed -- each board is strictly
/// single-in-flight, so "last-seen tsf_us per key, overwritten forever" is
/// sufficient for the lifetime of one boot (same reasoning already applied
/// to race-timer.h's g_run_start_tsf_us).
inline bool gate_event_is_duplicate(const char *gate_id, const char *event, uint64_t tsf_us) {
  for (size_t i = 0; i < DEDUP_TABLE_SIZE; i++) {
    if (strcmp(g_dedup_table[i].gate_id, gate_id) == 0 && strcmp(g_dedup_table[i].event, event) == 0) {
      bool is_dup = (g_dedup_table[i].last_tsf_us == tsf_us);
      g_dedup_table[i].last_tsf_us = tsf_us;
      return is_dup;
    }
  }
  // Not found -- new (gate_id, event) pair, round-robin into the next slot.
  DedupEntry &slot = g_dedup_table[g_dedup_next_slot];
  strncpy(slot.gate_id, gate_id, sizeof(slot.gate_id) - 1);
  slot.gate_id[sizeof(slot.gate_id) - 1] = '\0';
  strncpy(slot.event, event, sizeof(slot.event) - 1);
  slot.event[sizeof(slot.event) - 1] = '\0';
  slot.last_tsf_us = tsf_us;
  g_dedup_next_slot = (g_dedup_next_slot + 1) % DEDUP_TABLE_SIZE;
  return false;
}
