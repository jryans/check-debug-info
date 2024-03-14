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

  // Execution
  size_t functionCovered;
  size_t executionComplete;
  size_t withinTimeLimit;
  size_t withinForkLimit;
};

#endif // STATS_H
