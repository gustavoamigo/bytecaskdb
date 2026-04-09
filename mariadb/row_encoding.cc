// row_encoding.cc — Row encoding implementation (Phase 1: raw buffer copy).

#include "row_encoding.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions

#include <algorithm>
#include <cstring>

namespace bytecaskdb {

std::vector<uint8_t> encode_row(TABLE *table, const uchar *buf) {
  const std::size_t len = table->s->reclength;
  std::vector<uint8_t> out(len);
  std::memcpy(out.data(), buf, len);
  return out;
}

void decode_row(TABLE *table, const uint8_t *value, std::size_t value_len,
                uchar *buf) {
  const std::size_t reclength = table->s->reclength;
  const std::size_t copy_len  = std::min(value_len, reclength);

  if (copy_len > 0) {
    std::memcpy(buf, value, copy_len);
  }

  // Zero-pad if the stored value is shorter than the current reclength
  // (defensive: schema changes are not supported in Phase 1, so this should
  // not happen in practice).
  if (copy_len < reclength) {
    std::memset(buf + copy_len, 0, reclength - copy_len);
  }
}

} // namespace bytecaskdb
