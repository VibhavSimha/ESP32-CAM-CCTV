#include "resilience_policy.h"

void failureStateRecord(FailureState *state, unsigned long nowMs) {
  if (!state) return;
  state->streak++;
  if (state->firstFailureAtMs == 0) {
    state->firstFailureAtMs = nowMs;
  }
}

void failureStateReset(FailureState *state) {
  if (!state) return;
  state->streak = 0;
  state->firstFailureAtMs = 0;
}

bool shouldEscalateAfterSustainedFailures(const FailureState &state,
                                          unsigned long nowMs,
                                          uint16_t minStreak,
                                          unsigned long minDurationMs,
                                          unsigned long *downForOut) {
  unsigned long downFor = 0;
  if (state.firstFailureAtMs != 0 && nowMs >= state.firstFailureAtMs) {
    downFor = nowMs - state.firstFailureAtMs;
  }
  if (downForOut) *downForOut = downFor;

  return state.firstFailureAtMs != 0 &&
         state.streak >= minStreak &&
         downFor >= minDurationMs;
}

StreamRecoveryStage selectStreamRecoveryStage(const FailureState &state,
                                              unsigned long nowMs,
                                              uint8_t attemptsCompleted,
                                              uint16_t rebootAfterCount,
                                              unsigned long rebootAfterMs,
                                              uint8_t httpRestartAttempts) {
  if (shouldEscalateAfterSustainedFailures(state,
                                           nowMs,
                                           rebootAfterCount,
                                           rebootAfterMs,
                                           nullptr)) {
    return STREAM_RECOVERY_STAGE_REBOOT;
  }
  if (attemptsCompleted < httpRestartAttempts) {
    return STREAM_RECOVERY_STAGE_HTTP_RESTART;
  }
  return STREAM_RECOVERY_STAGE_CAMERA_REINIT;
}

bool streamSessionLooksHealthy(uint32_t frameCount, uint32_t minHealthyFrameCount) {
  return frameCount >= minHealthyFrameCount;
}
