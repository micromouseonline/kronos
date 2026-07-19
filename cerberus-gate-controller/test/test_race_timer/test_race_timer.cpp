#include <unity.h>

#include "race/race-timer.h"

namespace {

void reset_race_timer_state() {
  race_run_count = 0;
  mouse_run_count = 0;
  mouse_id = 0;
  mouse_first_run_index = 0;
  race_state = RaceState::CALIBRATE;
  time_left = RACE_TIME_LIMIT;
  run_sw.reset();
  entry_sw.reset();
  g_mock_millis = 0;
}

int state_of(RaceState state) {
  return static_cast<int>(state);
}

}  // namespace

#define ASSERT_STATE_EQ(expected) TEST_ASSERT_EQUAL(state_of(expected), state_of(race_timer_get_state()))

void setUp(void) {
  reset_race_timer_state();
}

void tearDown(void) {
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

void test_format_time_zero(void) {
  char buf[16];
  race_timer_format_time(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:00.000", buf);
}

void test_format_time_sub_minute(void) {
  char buf[16];
  race_timer_format_time(1234, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:01.234", buf);
}

void test_format_time_minute_boundary(void) {
  char buf[16];
  race_timer_format_time(61234, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("01:01.234", buf);
}

void test_format_time_seconds_variant(void) {
  char buf[16];
  race_timer_format_time_seconds(61234, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("01:01", buf);
}

void test_format_time_seconds_rounds_down(void) {
  char buf[16];
  race_timer_format_time_seconds(59999, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("00:59", buf);
}

// ---------------------------------------------------------------------------
// Leaderboard
// ---------------------------------------------------------------------------

void test_leaderboard_empty(void) {
  LeaderboardEntry out[MAX_RESULTS];
  size_t n = race_timer_compute_leaderboard(out, MAX_RESULTS);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)n);
}

void test_leaderboard_single_mouse_best_of(void) {
  race_runs[0] = {1, 1, 1000};
  race_runs[1] = {1, 2, 900};
  race_runs[2] = {1, 3, 1100};
  race_run_count = 3;

  LeaderboardEntry out[MAX_RESULTS];
  size_t n = race_timer_compute_leaderboard(out, MAX_RESULTS);

  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT16(1, out[0].mouse_id);
  TEST_ASSERT_EQUAL_UINT32(900, out[0].best_time_ms);
}

void test_leaderboard_multiple_mice_sorted_fastest_first(void) {
  race_runs[0] = {1, 1, 1000};
  race_runs[1] = {1, 2, 1200};
  race_runs[2] = {2, 1, 800};
  race_runs[3] = {2, 2, 750};
  race_run_count = 4;

  LeaderboardEntry out[MAX_RESULTS];
  size_t n = race_timer_compute_leaderboard(out, MAX_RESULTS);

  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT16(2, out[0].mouse_id);
  TEST_ASSERT_EQUAL_UINT32(750, out[0].best_time_ms);
  TEST_ASSERT_EQUAL_UINT16(1, out[1].mouse_id);
  TEST_ASSERT_EQUAL_UINT32(1000, out[1].best_time_ms);
}

void test_leaderboard_ties_keep_mouse_order(void) {
  race_runs[0] = {1, 1, 900};
  race_runs[1] = {2, 1, 900};
  race_run_count = 2;

  LeaderboardEntry out[MAX_RESULTS];
  size_t n = race_timer_compute_leaderboard(out, MAX_RESULTS);

  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT16(1, out[0].mouse_id);
  TEST_ASSERT_EQUAL_UINT16(2, out[1].mouse_id);
}

void test_leaderboard_max_out_truncates_by_mouse_order(void) {
  // 3 distinct mice, but max_out only leaves room for 2 candidates. The
  // function stops collecting once candidate_count hits max_out, so the
  // result is "first 2 mice encountered" sorted between themselves -- not
  // necessarily the true fastest 2 across all 3 (mouse 3's 100ms is fastest
  // overall but never considered).
  race_runs[0] = {1, 1, 1000};
  race_runs[1] = {2, 1, 500};
  race_runs[2] = {3, 1, 100};
  race_run_count = 3;

  LeaderboardEntry out[2];
  size_t n = race_timer_compute_leaderboard(out, 2);

  TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)n);
  TEST_ASSERT_EQUAL_UINT16(2, out[0].mouse_id);
  TEST_ASSERT_EQUAL_UINT16(1, out[1].mouse_id);
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void test_calibrate_new_mouse_enters_waiting(void) {
  ASSERT_STATE_EQ(RaceState::CALIBRATE);
  race_timer_handle_event(EV_NEW_MOUSE);
  ASSERT_STATE_EQ(RaceState::WAITING);
  TEST_ASSERT_EQUAL_UINT16(1, mouse_id);
  TEST_ASSERT_EQUAL_UINT16(0, mouse_run_count);
}

void test_waiting_arm_enters_armed(void) {
  race_timer_handle_event(EV_NEW_MOUSE);  // -> WAITING
  race_timer_handle_event(EV_ARM);
  ASSERT_STATE_EQ(RaceState::ARMED);
}

void test_waiting_restart_reenters_new_mouse(void) {
  race_timer_handle_event(EV_NEW_MOUSE);  // -> WAITING, mouse_id == 1
  race_timer_handle_event(EV_RESTART);
  ASSERT_STATE_EQ(RaceState::WAITING);
  TEST_ASSERT_EQUAL_UINT16(2, mouse_id);
}

void test_armed_start_enters_running(void) {
  race_timer_handle_event(EV_NEW_MOUSE);
  race_timer_handle_event(EV_ARM);
  g_mock_millis = 100;
  race_timer_handle_event(EV_START);
  ASSERT_STATE_EQ(RaceState::RUNNING);
  TEST_ASSERT_EQUAL_UINT16(1, mouse_run_count);
}

void test_running_goal_commits_run_and_enters_goal(void) {
  race_timer_handle_event(EV_NEW_MOUSE);
  race_timer_handle_event(EV_ARM);
  g_mock_millis = 100;
  race_timer_handle_event(EV_START);
  g_mock_millis = 1600;
  race_timer_handle_event(EV_GOAL);

  ASSERT_STATE_EQ(RaceState::GOAL);
  TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)race_run_count);
  TEST_ASSERT_EQUAL_UINT16(mouse_id, race_runs[0].mouse_id);
  TEST_ASSERT_EQUAL_UINT16(1, race_runs[0].run_number);
  TEST_ASSERT_EQUAL_UINT32(1500, race_runs[0].time_ms);
}

void test_running_arm_abandons_without_commit(void) {
  race_timer_handle_event(EV_NEW_MOUSE);
  race_timer_handle_event(EV_ARM);
  g_mock_millis = 100;
  race_timer_handle_event(EV_START);
  race_timer_handle_event(EV_ARM);  // manual recovery, abandons the run

  ASSERT_STATE_EQ(RaceState::ARMED);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)race_run_count);
}

void test_running_restart_abandons_and_reenters_new_mouse(void) {
  race_timer_handle_event(EV_NEW_MOUSE);
  race_timer_handle_event(EV_ARM);
  race_timer_handle_event(EV_START);
  race_timer_handle_event(EV_RESTART);

  ASSERT_STATE_EQ(RaceState::WAITING);
  TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)race_run_count);
  TEST_ASSERT_EQUAL_UINT16(2, mouse_id);
}

void test_goal_restart_reenters_new_mouse(void) {
  race_timer_handle_event(EV_NEW_MOUSE);
  race_timer_handle_event(EV_ARM);
  race_timer_handle_event(EV_START);
  race_timer_handle_event(EV_GOAL);
  race_timer_handle_event(EV_RESTART);

  ASSERT_STATE_EQ(RaceState::WAITING);
  TEST_ASSERT_EQUAL_UINT16(2, mouse_id);
}

void test_ev_none_is_always_a_noop(void) {
  race_timer_handle_event(EV_NEW_MOUSE);  // -> WAITING
  RaceState before = race_timer_get_state();
  race_timer_handle_event(EV_NONE);
  ASSERT_STATE_EQ(before);
}

void test_mouse_exhausted_after_max_runs_drops_to_waiting(void) {
  race_timer_handle_event(EV_NEW_MOUSE);  // -> WAITING

  for (size_t i = 0; i < MAX_RUNS_PER_MOUSE; i++) {
    race_timer_handle_event(EV_ARM);
    ASSERT_STATE_EQ(RaceState::ARMED);
    race_timer_handle_event(EV_START);
    g_mock_millis += 100;
    race_timer_handle_event(EV_GOAL);
    ASSERT_STATE_EQ(RaceState::GOAL);
  }

  TEST_ASSERT_TRUE(race_timer_mouse_exhausted());

  // One more ARM must not arm -- an exhausted mouse drops back to WAITING.
  race_timer_handle_event(EV_ARM);
  ASSERT_STATE_EQ(RaceState::WAITING);
  TEST_ASSERT_EQUAL_UINT32((uint32_t)MAX_RUNS_PER_MOUSE, (uint32_t)race_run_count);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  UNITY_BEGIN();

  RUN_TEST(test_format_time_zero);
  RUN_TEST(test_format_time_sub_minute);
  RUN_TEST(test_format_time_minute_boundary);
  RUN_TEST(test_format_time_seconds_variant);
  RUN_TEST(test_format_time_seconds_rounds_down);

  RUN_TEST(test_leaderboard_empty);
  RUN_TEST(test_leaderboard_single_mouse_best_of);
  RUN_TEST(test_leaderboard_multiple_mice_sorted_fastest_first);
  RUN_TEST(test_leaderboard_ties_keep_mouse_order);
  RUN_TEST(test_leaderboard_max_out_truncates_by_mouse_order);

  RUN_TEST(test_calibrate_new_mouse_enters_waiting);
  RUN_TEST(test_waiting_arm_enters_armed);
  RUN_TEST(test_waiting_restart_reenters_new_mouse);
  RUN_TEST(test_armed_start_enters_running);
  RUN_TEST(test_running_goal_commits_run_and_enters_goal);
  RUN_TEST(test_running_arm_abandons_without_commit);
  RUN_TEST(test_running_restart_abandons_and_reenters_new_mouse);
  RUN_TEST(test_goal_restart_reenters_new_mouse);
  RUN_TEST(test_ev_none_is_always_a_noop);
  RUN_TEST(test_mouse_exhausted_after_max_runs_drops_to_waiting);

  return UNITY_END();
}
