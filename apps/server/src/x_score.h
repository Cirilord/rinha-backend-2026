#ifndef X_SCORE_H
#define X_SCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define X_SCORE_DIMS 14
#define X_SCORE_LANES 8
#define X_SCORE_TOPK 5
#define X_SCORE_SCALE 10000
#define X_SCORE_MAGIC "RNSPCST1"

typedef struct {
  char magic[8];
  int32_t scale;
  int32_t dims;
  int32_t count;
  int32_t partition_count;
  int32_t node_count;
  int32_t block_count;
} __attribute__((packed)) XScoreIndexHeader;

_Static_assert(sizeof(XScoreIndexHeader) == 32, "XScoreIndexHeader must be 32 bytes");

typedef struct {
  uint32_t key;
  int32_t root;
  int32_t start;
  int32_t len;
  int16_t min[X_SCORE_DIMS];
  int16_t max[X_SCORE_DIMS];
} __attribute__((packed)) XScorePartitionEntry;

_Static_assert(sizeof(XScorePartitionEntry) == 72, "XScorePartitionEntry must be 72 bytes");

typedef struct {
  int32_t left;
  int32_t right;
  int32_t start_block;
  int32_t len;
  int16_t min[X_SCORE_DIMS];
  int16_t max[X_SCORE_DIMS];
} __attribute__((packed)) XScoreNodeEntry;

_Static_assert(sizeof(XScoreNodeEntry) == 72, "XScoreNodeEntry must be 72 bytes");

typedef struct {
  uint32_t start_block;
  uint16_t block_count;
  uint16_t len;
} XScoreBlockGroup;

typedef struct {
  uint8_t *raw;
  size_t size;
  int mapped;
  const XScoreIndexHeader *header;
  const XScorePartitionEntry *partitions;
  const XScoreNodeEntry *nodes;
  const int16_t *vectors_q16;
  const uint8_t *labels;
  uint32_t *key_partition_indices;
  uint32_t key_partition_offsets[257];
  int32_t key_partition_direct[256];
  uint32_t *partition_start_blocks;
  uint32_t *partition_lens;
  uint32_t *partition_group_offsets;
  XScoreBlockGroup *groups;
  uint32_t total_groups;
  uint32_t max_groups_per_partition;
  uint8_t direct_leaf_mode;
  uint32_t count;
  uint32_t partition_count;
  uint32_t node_count;
  uint32_t block_count;
} XScoreIndexView;

bool x_score_open(const char *path, XScoreIndexView *out_view);
void x_score_close(XScoreIndexView *view);
uint8_t x_score_predict_fraud_count(const XScoreIndexView *view,
                                    const double query[X_SCORE_DIMS]);
uint8_t x_score_predict_label(const XScoreIndexView *view,
                              const double query[X_SCORE_DIMS]);

#endif
