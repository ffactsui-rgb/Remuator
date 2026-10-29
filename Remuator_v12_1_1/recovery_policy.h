#pragma once
#include <stdint.h>

// The saved checkboxes are authoritative. A completed axis is never replayed.
enum class RecoveryAction : uint8_t { Tilt, Rotate, Wait, Next, Finish };
struct RecoveryDecision {
  RecoveryAction action;
  uint64_t remainingUs;
  bool reanchor;
};
inline RecoveryDecision decideRecovery(bool tiltDone, bool rotationDone, bool lastStep,
                                       int64_t nextEpoch, int64_t nowEpoch, uint32_t pauseSeconds) {
  const bool overdue = nextEpoch <= nowEpoch;
  const uint64_t remaining = overdue ? 0 : uint64_t(nextEpoch - nowEpoch) * 1000000ULL;
  if (!tiltDone || !rotationDone)
    return {!tiltDone ? RecoveryAction::Tilt : RecoveryAction::Rotate,
            overdue ? uint64_t(pauseSeconds) * 1000000ULL : remaining, overdue};
  if (!overdue)
    return {RecoveryAction::Wait, remaining, false};
  return {lastStep ? RecoveryAction::Finish : RecoveryAction::Next, 0, true};
}
