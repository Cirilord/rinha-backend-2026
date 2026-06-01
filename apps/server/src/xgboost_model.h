#ifndef XGBOOST_MODEL_H
#define XGBOOST_MODEL_H

#include <stdint.h>

#define XGBOOST_MODEL_DIMS 14

float xgboost_predict_probability(const double features[XGBOOST_MODEL_DIMS]);
uint8_t xgboost_predict_fraud_count(const double features[XGBOOST_MODEL_DIMS]);

#endif
