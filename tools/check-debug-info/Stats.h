#ifndef STATS_H
#define STATS_H

// Summary stats for all assignments encountered
struct AssignmentStats {
  unsigned int refTotal;
  unsigned int total;
  unsigned int matchingCoords;
  unsigned int matchingValue;

  // Errors
  unsigned int mismatchedCoords;
  unsigned int mismatchedValue;
  unsigned int notEncountered;
  unsigned int missing;

  // Warnings
  unsigned int unused;
  unsigned int unreachable;
  unsigned int removable;

  // Execution
  unsigned int functionCovered;
  unsigned int executionComplete;
  unsigned int withinTimeLimit;
  unsigned int withinForkLimit;
};

#endif // STATS_H
