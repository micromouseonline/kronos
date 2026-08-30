/*
 * File:   stopwatch.h
 * Author: peterharrison
 * Created from http://playground.arduino.cc/Code/StopWatchClass
 * Created on 14 June 2015, 09:11
 */

#pragma once

#include <Arduino.h>

class Stopwatch {
  public:
  enum State { STOPPED, RUNNING, RESET, WAITING };
  Stopwatch() : mState(RESET), mTime(0) {
    reset();
  };

  ~Stopwatch() = default;

  void reset() {
    mSplitTime = 0;
    mLapTime = 0;
    mStartMillis = millis();
    mStopMillis = mStartMillis;
    mState = Stopwatch::RESET;
  }

  void restart(uint32_t timestamp = millis()) {
    reset();
    mStartMillis = timestamp;
    mState = Stopwatch::RUNNING;
  }

  void stop(uint32_t timestamp = millis()) {
    if (mState == Stopwatch::RUNNING) {
      mStopMillis = timestamp;
      mTime = mStopMillis - mStartMillis;
      mState = Stopwatch::STOPPED;
    }
  }

  void resume() {
    if (mState == STOPPED) {
      mState = RUNNING;
      mStartMillis = millis() - mTime;  // Preserve elapsed time
    }
  }

  bool running() {
    return mState == RUNNING;
  }

  uint32_t time() {
    switch (mState) {
      case RUNNING:
        return millis() - mStartMillis;
      case STOPPED:
        return mTime;  // frozen at stop()
      case RESET:
      default:
        return 0;
    }
  }

  /// methods for debugging
  bool isReset() const {
    return mState == Stopwatch::RESET;
  }

  Stopwatch::State getState() const {
    return mState;
  }

  uint32_t getStartTime() const {
    return mStartMillis;
  }

  /**
 * @brief Captures the duration since the last lap or restart, then resets the internal timer.
 *
 * This method is typically used to measure the time taken to complete a segment or interval
 * during a running session. It returns the elapsed time since the previous lap() call or since
 * the stopwatch was restarted. After computing the lap time, it resets the internal start time
 * so that the next lap measures from this point forward.

 * Example use:
 * stopwatch.restart();  // Begin timing
 * ...
 * uint32_t lap1 = stopwatch.lap();   // After first segment
 * ...
 * uint32_t lap2 = stopwatch.lap();   // After second segment
 *
 * Serial.printf("Segment 1: %lu ms\n", lap1);
 * Serial.printf("Segment 2: %lu ms\n", lap2);
 *
 * @return Lap duration in milliseconds since last lap or restart.
 */

  uint32_t lap() {
    if (mState == Stopwatch::RUNNING) {
      mStopMillis = millis();
      mLapTime = (mStopMillis - mStartMillis);
      mStartMillis = mStopMillis;
    }
    return mLapTime;
  }

  /**
 * @brief Measures the duration since the last restart without resetting the internal timer.
 *
 * This method is useful for checkpointing or intermediate progress logging during an active
 * timer run. Unlike lap(), split() does not affect the ongoing stopwatch state or timing base.
 * It simply returns the current elapsed time since the last restart.

 * Example use:
 * stopwatch.restart();  // Begin timing
 * ...
 * uint32_t firstSplit = stopwatch.split();  // Checkpoint reached
 * ...
 * uint32_t secondSplit = stopwatch.split(); // Later checkpoint
 *
 * Serial.printf("Time since start: %lu ms\n", firstSplit);
 * Serial.printf("Time since start: %lu ms\n", secondSplit);
 *
 * @return Split time in milliseconds since last restart.
 */

  uint32_t split() {
    if (mState == Stopwatch::RUNNING) {
      mStopMillis = millis();
      mSplitTime = (mStopMillis - mStartMillis);
    }
    return mSplitTime;
  }

  private:
  enum State mState;
  uint32_t mStartMillis;
  uint32_t mStopMillis;
  uint32_t mTime;
  uint32_t mLapTime;
  uint32_t mSplitTime;
};
