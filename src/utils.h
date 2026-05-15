#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdbool.h>

// Extract pathname from absolute or relative URL.
// Examples:
// - "http://localhost:9999/fraud-score?x=1" -> "/fraud-score"
// - "/fraud-score?x=1" -> "/fraud-score"
// Returns 1 on success, 0 on failure/truncation.
int extract_pathname(const char *url, char *out, size_t out_size);

// Find JSON value by dot notation path (e.g. "id", "transaction.amount").
// Writes the value to `out`:
// - string values are written without quotes
// - number/boolean/null are written as raw token text
// Returns 1 on success, 0 on not found/invalid/truncation.
int find_value(const char *body, const char *path, char *out, size_t out_size);

int to_int(const char *value, int *out);
int to_double(const char *value, double *out);
int to_bool(const char *value, bool *out);
int to_epoch_time(const char *iso, long long *out); // UTC seconds since Unix epoch
double clamp_01(double value);
int day_of_week_from_epoch(long long epoch_seconds);
int hour_of_day_from_epoch(long long epoch_seconds); // UTC hour: 0..23

#endif
