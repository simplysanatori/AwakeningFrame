// AwakeningFrame: crash reports the player chooses to send (runtime/crash_report.c, docs/TECHNICAL.md "Crash
// reports"). Linux only; elsewhere these do nothing and nothing is ever pending.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void crash_report_init(void);    // main(), after rt_paths_init: this build's release name, the last run's record
void crash_report_record(void);  // rt_fatal_trace: this run's record, for the next start
void crash_report_test_arm(void);  // AF_SENTRY_TEST_CRASH=1: a real crash 10 s after the game starts (a test)
// A record from the last run that this build can send (it has a Sentry DSN), not answered yet
bool crash_report_pending(void);
const char *crash_report_reason(void);  // what went wrong, in plain words
const char *crash_report_text(void);    // all of what Send Report sends (See What's Sent)
void crash_report_send(void);           // Send Report: in the background
// 0 nothing sent yet, 1 sending, 2 sent, 3 failed (msg: why)
int crash_report_state(char *msg, size_t n);
void crash_report_done(void);           // answered (Don't Send, or after sending): pending no more

#ifdef __cplusplus
}
#endif
