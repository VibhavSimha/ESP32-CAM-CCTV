// Host unit tests for stream/cloud resilience policy decisions.
//
// Build & run on a normal machine (no ESP32 toolchain):
//   c++ -std=c++11 -I.. test_resilience_policy.cpp ../resilience_policy.cpp -o /tmp/t && /tmp/t

#include "resilience_policy.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_failure_state_record_sets_timestamp_once() {
    std::printf("test_failure_state_record_sets_timestamp_once\n");
    FailureState s = {0, 0};
    failureStateRecord(&s, 1000);
    CHECK(s.streak == 1);
    CHECK(s.firstFailureAtMs == 1000);
    failureStateRecord(&s, 2500);
    CHECK(s.streak == 2);
    CHECK(s.firstFailureAtMs == 1000);
}

static void test_failure_state_reset_clears_fields() {
    std::printf("test_failure_state_reset_clears_fields\n");
    FailureState s = {12, 5000};
    failureStateReset(&s);
    CHECK(s.streak == 0);
    CHECK(s.firstFailureAtMs == 0);
}

static void test_escalation_requires_both_count_and_duration() {
    std::printf("test_escalation_requires_both_count_and_duration\n");
    FailureState s = {5, 1000};
    unsigned long downFor = 0;

    CHECK(!shouldEscalateAfterSustainedFailures(s, 1500, 5, 1000, &downFor));
    CHECK(downFor == 500);

    CHECK(!shouldEscalateAfterSustainedFailures(s, 2500, 6, 1000, &downFor));
    CHECK(downFor == 1500);
}

static void test_escalation_when_thresholds_met() {
    std::printf("test_escalation_when_thresholds_met\n");
    FailureState s = {8, 1000};
    unsigned long downFor = 0;
    CHECK(shouldEscalateAfterSustainedFailures(s, 2600, 8, 1600, &downFor));
    CHECK(downFor == 1600);
}

static void test_escalation_ignores_uninitialized_timestamp() {
    std::printf("test_escalation_ignores_uninitialized_timestamp\n");
    FailureState s = {100, 0};
    unsigned long downFor = 1234;
    CHECK(!shouldEscalateAfterSustainedFailures(s, 9000, 1, 1, &downFor));
    CHECK(downFor == 0);
}

static void test_stream_recovery_stage_http_then_camera() {
    std::printf("test_stream_recovery_stage_http_then_camera\n");
    FailureState s = {1, 1000};
    CHECK(selectStreamRecoveryStage(s, 1200, 0, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_HTTP_RESTART);
    CHECK(selectStreamRecoveryStage(s, 1200, 1, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_HTTP_RESTART);
    CHECK(selectStreamRecoveryStage(s, 1200, 2, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_CAMERA_REINIT);
    CHECK(selectStreamRecoveryStage(s, 1200, 9, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_CAMERA_REINIT);
}

static void test_stream_recovery_stage_reboot_preempts_other_actions() {
    std::printf("test_stream_recovery_stage_reboot_preempts_other_actions\n");
    FailureState s = {20, 1000};
    CHECK(selectStreamRecoveryStage(s, 901000, 0, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_REBOOT);
    CHECK(selectStreamRecoveryStage(s, 901000, 5, 20, 900000, 2) ==
          STREAM_RECOVERY_STAGE_REBOOT);
}

static void test_stream_session_healthy_gate() {
    std::printf("test_stream_session_healthy_gate\n");
    CHECK(!streamSessionLooksHealthy(29, 30));
    CHECK(streamSessionLooksHealthy(30, 30));
    CHECK(streamSessionLooksHealthy(31, 30));
}

int main() {
    test_failure_state_record_sets_timestamp_once();
    test_failure_state_reset_clears_fields();
    test_escalation_requires_both_count_and_duration();
    test_escalation_when_thresholds_met();
    test_escalation_ignores_uninitialized_timestamp();
    test_stream_recovery_stage_http_then_camera();
    test_stream_recovery_stage_reboot_preempts_other_actions();
    test_stream_session_healthy_gate();

    if (g_failures == 0) {
        std::printf("\nAll resilience policy tests passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
