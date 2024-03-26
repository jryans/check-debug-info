#ifndef STATS_H
#define STATS_H

// Summary stats for all assignments encountered
struct AssignmentStats {
  size_t refTotal;
  size_t testTotal;

  // Matching
  size_t matchingCoords;
  size_t matchingValue;

  // Consistency Errors
  size_t mismatchedCoords;
  size_t mismatchedValue;

  // For non-consistency errors and warnings below,
  // an assignment is added to only one of these buckets.

  // Availability Errors
  // Not encountered during reference execution
  size_t refNotEncountered;
  // Reference assignment not found in test
  size_t refNotInTest;
  // Not encountered during test execution
  size_t testNotEncountered;
  // Test assignment not found in reference
  size_t testNotInRef;

  // Warnings
  // Optional diagnostics file claims reference variable is unused
  size_t unused;
  // All reference assignments statically removable
  size_t removable;
  // Not encountered during (complete but uncovered) reference execution
  size_t unreachable;

  // Reference Execution
  size_t refFunctionCovered;
  size_t refExecutionComplete;
  size_t refWithinTimeLimit;
  size_t refWithinForkLimit;

  // Test Execution
  size_t testFunctionCovered;
  size_t testExecutionComplete;
  size_t testWithinTimeLimit;
  size_t testWithinForkLimit;

  void operator+=(const AssignmentStats &other) {
    refTotal += other.refTotal;
    testTotal += other.testTotal;
    matchingCoords += other.matchingCoords;
    matchingValue += other.matchingValue;
    mismatchedCoords += other.mismatchedCoords;
    mismatchedValue += other.mismatchedValue;
    refNotEncountered += other.refNotEncountered;
    refNotInTest += other.refNotInTest;
    testNotEncountered += other.testNotEncountered;
    testNotInRef += other.testNotInRef;
    unused += other.unused;
    removable += other.removable;
    unreachable += other.unreachable;
    refFunctionCovered += other.refFunctionCovered;
    refExecutionComplete += other.refExecutionComplete;
    refWithinTimeLimit += other.refWithinTimeLimit;
    refWithinForkLimit += other.refWithinForkLimit;
    testFunctionCovered += other.testFunctionCovered;
    testExecutionComplete += other.testExecutionComplete;
    testWithinTimeLimit += other.testWithinTimeLimit;
    testWithinForkLimit += other.testWithinForkLimit;
  }
};

#endif // STATS_H
