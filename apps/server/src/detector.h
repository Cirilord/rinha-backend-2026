#ifndef DETECTOR_H
#define DETECTOR_H

#include "x-score.h"

typedef XScoreIndexView DetectorIndexView;

static inline bool detector_open(const char *path, DetectorIndexView *out_view) {
  return x_score_open(path, out_view);
}

static inline void detector_close(DetectorIndexView *view) {
  x_score_close(view);
}

static inline uint8_t detector_predict_fraud_count(const DetectorIndexView *view,
                                                   const double query[X_SCORE_DIMS]) {
  return x_score_predict_fraud_count(view, query);
}

static inline uint8_t detector_predict_label(const DetectorIndexView *view,
                                             const double query[X_SCORE_DIMS]) {
  return x_score_predict_label(view, query);
}

#endif
