#pragma once

#include <stdint.h>

struct FailureState {
  uint16_t streak;
  unsigned long firstFailureAtMs;
};

enum StreamRecoveryStage {
  STREAM_RECOVERY_STAGE_HTTP_RESTART = 0,
  STREAM_RECOVERY_STAGE_CAMERA_REINIT = 1,
  STREAM_RECOVERY_STAGE_REBOOT = 2
};

void failureStateRecord(FailureState *state, unsigned long nowMs);
void failureStateReset(FailureState *state);

bool shouldEscalateAfterSustainedFailures(const FailureState &state,
                                          unsigned long nowMs,
                                          uint16_t minStreak,
                                          unsigned long minDurationMs,
                                          unsigned long *downForOut);

StreamRecoveryStage selectStreamRecoveryStage(const FailureState &state,
                                              unsigned long nowMs,
                                              uint8_t attemptsCompleted,
                                              uint16_t rebootAfterCount,
                                              unsigned long rebootAfterMs,
                                              uint8_t httpRestartAttempts);

bool streamSessionLooksHealthy(uint32_t frameCount, uint32_t minHealthyFrameCount);
