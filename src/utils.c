#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
  while (*p && isspace((unsigned char)*p)) p++;
  return p;
}

double clamp_01(double value) {
  if (value <= 0.0) return 0.0;
  if (value >= 1.0) return 1.0;
  return value;
}

static int get_day_of_week_ymd(int year, int month, int day) {
  static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) {
    year--;
  }
  return (year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day + 6) % 7;
}

int day_of_week_from_epoch(long long epoch_seconds) {
  // Convert epoch days to civil date (UTC), then reuse the Y/M/D weekday formula.
  long long days = epoch_seconds / 86400LL;
  long long z = days + 719468LL;
  long long era = (z >= 0 ? z : z - 146096LL) / 146097LL;
  long long doe = z - era * 146097LL;
  long long yoe = (doe - doe / 1460LL + doe / 36524LL - doe / 146096LL) / 365LL;
  long long y = yoe + era * 400LL;
  long long doy = doe - (365LL * yoe + yoe / 4LL - yoe / 100LL);
  long long mp = (5LL * doy + 2LL) / 153LL;
  long long d = doy - (153LL * mp + 2LL) / 5LL + 1LL;
  long long m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);

  return get_day_of_week_ymd((int)y, (int)m, (int)d);
}

int hour_of_day_from_epoch(long long epoch_seconds) {
  long long seconds_in_day = epoch_seconds % 86400LL;
  if (seconds_in_day < 0) {
    seconds_in_day += 86400LL;
  }
  return (int)(seconds_in_day / 3600LL);
}

int extract_pathname(const char *url, char *out, size_t out_size) {
  size_t i = 0;
  size_t start = 0;
  size_t end = 0;

  if (!url || !out || out_size == 0) {
    return 0;
  }

  // Relative URL: "/path?query"
  if (url[0] == '/') {
    start = 0;
  } else {
    // Absolute URL: "http://host/path?query"
    const char *scheme = strstr(url, "://");
    if (!scheme) {
      return 0;
    }

    const char *path = strchr(scheme + 3, '/');
    if (!path) {
      if (out_size < 2) return 0;
      out[0] = '/';
      out[1] = '\0';
      return 1;
    }
    start = (size_t)(path - url);
  }

  end = start;
  while (url[end] != '\0' && url[end] != '?') {
    end++;
  }

  if (end <= start) {
    if (out_size < 2) return 0;
    out[0] = '/';
    out[1] = '\0';
    return 1;
  }

  if ((end - start + 1) > out_size) {
    return 0;
  }

  for (i = 0; i < (end - start); i++) {
    out[i] = url[start + i];
  }
  out[i] = '\0';

  return 1;
}

int find_value(const char *body, const char *path, char *out, size_t out_size) {
  const char *cursor;
  const char *seg_start;
  const char *dot;
  const char *value_pos;
  const char *p;
  const char *end;
  size_t w = 0;
  const char array_sep = '\x1F'; // safer separator: ASCII Unit Separator

  if (!body || !path || !out || out_size == 0 || *path == '\0') return 0;
  out[0] = '\0';

  cursor = body;
  seg_start = path;
  value_pos = NULL;

  while (1) {
    dot = strchr(seg_start, '.');
    size_t seg_len = dot ? (size_t)(dot - seg_start) : strlen(seg_start);
    if (seg_len == 0) return 0;

    p = skip_ws(cursor);
    if (*p != '{') return 0;
    p++;

    value_pos = NULL;
    while (*p) {
      const char *k_start;
      const char *k_end;
      size_t key_len;

      p = skip_ws(p);
      if (*p == '}') break;
      if (*p != '"') return 0;

      p++;
      k_start = p;
      while (*p) {
        if (*p == '\\' && *(p + 1)) {
          p += 2;
          continue;
        }
        if (*p == '"') break;
        p++;
      }
      if (*p != '"') return 0;
      k_end = p;
      key_len = (size_t)(k_end - k_start);
      p++;

      p = skip_ws(p);
      if (*p != ':') return 0;
      p++;
      p = skip_ws(p);

      if (key_len == seg_len && memcmp(k_start, seg_start, seg_len) == 0) {
        value_pos = p;
        break;
      }

      // skip current value
      if (*p == '"') {
        p++;
        while (*p) {
          if (*p == '\\' && *(p + 1)) {
            p += 2;
            continue;
          }
          if (*p == '"') {
            p++;
            break;
          }
          p++;
        }
      } else if (*p == '{') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
          if (*p == '"') {
            p++;
            while (*p) {
              if (*p == '\\' && *(p + 1)) {
                p += 2;
                continue;
              }
              if (*p == '"') {
                p++;
                break;
              }
              p++;
            }
            continue;
          }
          if (*p == '{') depth++;
          else if (*p == '}') depth--;
          p++;
        }
      } else if (*p == '[') {
        int depth = 1;
        p++;
        while (*p && depth > 0) {
          if (*p == '"') {
            p++;
            while (*p) {
              if (*p == '\\' && *(p + 1)) {
                p += 2;
                continue;
              }
              if (*p == '"') {
                p++;
                break;
              }
              p++;
            }
            continue;
          }
          if (*p == '[') depth++;
          else if (*p == ']') depth--;
          p++;
        }
      } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
      }

      p = skip_ws(p);
      if (*p == ',') p++;
    }

    if (!value_pos) return 0;

    if (!dot) {
      value_pos = skip_ws(value_pos);

      // Object is not a valid return type for this helper.
      if (*value_pos == '{') {
        return 0;
      }

      // Array flatten: item1<US>item2<US>item3
      if (*value_pos == '[') {
        int first = 1;
        p = value_pos + 1;
        while (*p) {
          p = skip_ws(p);
          if (*p == ']') break;

          if (!first) {
            if (w + 1 >= out_size) return 0;
            out[w++] = array_sep;
          }
          first = 0;

          if (*p == '"') {
            p++;
            while (*p && *p != '"') {
              if (*p == '\\' && *(p + 1)) p++;
              if (w + 1 >= out_size) return 0;
              out[w++] = *p++;
            }
            if (*p != '"') return 0;
            p++;
          } else {
            const char *token_start = p;
            while (*p && *p != ',' && *p != ']' && !isspace((unsigned char)*p)) p++;
            end = p;
            while (end > token_start && isspace((unsigned char)*(end - 1))) end--;
            if ((size_t)(end - token_start) + w + 1 > out_size) return 0;
            memcpy(out + w, token_start, (size_t)(end - token_start));
            w += (size_t)(end - token_start);
          }

          p = skip_ws(p);
          if (*p == ',') p++;
        }
        out[w] = '\0';
        return 1;
      }

      // String value: return without quotes
      if (*value_pos == '"') {
        p = value_pos + 1;
        while (*p && *p != '"') {
          if (*p == '\\' && *(p + 1)) p++;
          if (w + 1 >= out_size) return 0;
          out[w++] = *p++;
        }
        if (*p != '"') return 0;
        out[w] = '\0';
        return 1;
      }

      // Scalar token: number / boolean / null
      p = value_pos;
      while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
      end = p;
      while (end > value_pos && isspace((unsigned char)*(end - 1))) end--;
      if ((size_t)(end - value_pos) + 1 > out_size) return 0;
      memcpy(out, value_pos, (size_t)(end - value_pos));
      out[end - value_pos] = '\0';
      return 1;
    }

    value_pos = skip_ws(value_pos);
    if (*value_pos != '{') return 0;
    cursor = value_pos;
    seg_start = dot + 1;
  }
}

int to_bool(const char *value, bool *out) {
  if (!value || !out) return 0;

  if (strcmp(value, "true") == 0) {
    *out = true;
    return 1;
  }
  if (strcmp(value, "false") == 0) {
    *out = false;
    return 1;
  }

  return 0;
}

int to_double(const char *value, double *out) {
  char *end = NULL;
  double parsed;

  if (!value || !out || *value == '\0') return 0;

  errno = 0;
  parsed = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0') return 0;

  *out = parsed;
  return 1;
}

int to_epoch_time(const char *iso, long long *out) {
  int y, mo, d, h, mi, s;
  int month_shifted;
  long long year, era, yoe, doy, doe, days_since_unix_epoch;

  if (!iso || !out) return 0;
  if (strlen(iso) != 20) return 0;
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' || iso[19] != 'Z') {
    return 0;
  }

  if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  if (mo < 1 || mo > 12) return 0;
  if (d < 1 || d > 31) return 0;
  if (h < 0 || h > 23) return 0;
  if (mi < 0 || mi > 59) return 0;
  if (s < 0 || s > 60) return 0;

  year = y;
  year -= (mo <= 2) ? 1 : 0;
  era = year / 400;
  if (year < 0 && (year % 400)) era--;
  yoe = year - era * 400;
  month_shifted = (mo > 2) ? (mo - 3) : (mo + 9);
  doy = (153LL * month_shifted + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days_since_unix_epoch = era * 146097 + doe - 719468;

  *out = days_since_unix_epoch * 86400LL + (long long)h * 3600LL + (long long)mi * 60LL + (long long)s;
  return 1;
}

int to_int(const char *value, int *out) {
  char *end = NULL;
  long parsed;

  if (!value || !out || *value == '\0') return 0;

  errno = 0;
  parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') return 0;

  *out = (int)parsed;
  return 1;
}
