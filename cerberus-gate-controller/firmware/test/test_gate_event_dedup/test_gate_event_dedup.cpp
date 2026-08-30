#include <unity.h>

#include <cstdio>

#include "net/gate-event-dedup.h"

namespace {

void reset_dedup_table() {
  for (auto &entry : g_dedup_table) {
    entry.gate_id[0] = '\0';
    entry.event[0] = '\0';
    entry.last_tsf_us = 0;
  }
  g_dedup_next_slot = 0;
}

}  // namespace

void setUp(void) {
  reset_dedup_table();
}

void tearDown(void) {
}

void test_first_sighting_is_not_a_duplicate(void) {
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
}

void test_exact_repeat_is_a_duplicate(void) {
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
  TEST_ASSERT_TRUE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
}

void test_new_tsf_on_same_key_is_not_a_duplicate(void) {
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 2000));
  // The new tsf_us is now what's remembered for this key.
  TEST_ASSERT_TRUE(gate_event_is_duplicate("GATE_A", "GOAL", 2000));
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
}

void test_different_gate_id_same_tsf_is_not_a_duplicate(void) {
  // Same numeric tsf_us can legitimately appear on two different boards --
  // TSF is a clock shared across every station on the AP.
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "GOAL", 1000));
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_B", "GOAL", 1000));
}

void test_different_event_same_gate_and_tsf_is_not_a_duplicate(void) {
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "ARM", 1000));
  TEST_ASSERT_FALSE(gate_event_is_duplicate("GATE_A", "START", 1000));
}

void test_table_wraparound_after_more_than_capacity_distinct_keys(void) {
  char gate_id[16];
  for (size_t i = 0; i < DEDUP_TABLE_SIZE + 4; i++) {
    snprintf(gate_id, sizeof(gate_id), "GATE_%zu", i);
    TEST_ASSERT_FALSE(gate_event_is_duplicate(gate_id, "GOAL", 1000 + i));
  }
  // The earliest keys have been round-robin evicted -- their tsf_us is no
  // longer remembered, so seeing them again reads as a fresh (non-duplicate)
  // sighting rather than a false-positive duplicate.
  snprintf(gate_id, sizeof(gate_id), "GATE_%zu", (size_t)0);
  TEST_ASSERT_FALSE(gate_event_is_duplicate(gate_id, "GOAL", 1000));

  // A recently-inserted key (within capacity of the last insert) is still
  // correctly recognised as a duplicate.
  snprintf(gate_id, sizeof(gate_id), "GATE_%zu", DEDUP_TABLE_SIZE + 3);
  TEST_ASSERT_TRUE(gate_event_is_duplicate(gate_id, "GOAL", 1000 + DEDUP_TABLE_SIZE + 3));
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();

  RUN_TEST(test_first_sighting_is_not_a_duplicate);
  RUN_TEST(test_exact_repeat_is_a_duplicate);
  RUN_TEST(test_new_tsf_on_same_key_is_not_a_duplicate);
  RUN_TEST(test_different_gate_id_same_tsf_is_not_a_duplicate);
  RUN_TEST(test_different_event_same_gate_and_tsf_is_not_a_duplicate);
  RUN_TEST(test_table_wraparound_after_more_than_capacity_distinct_keys);

  return UNITY_END();
}
