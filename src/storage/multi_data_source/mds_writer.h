#ifndef SRC_STORAGE_MULTI_DATA_SOURCE_MDS_WRITTER_H
#define SRC_STORAGE_MULTI_DATA_SOURCE_MDS_WRITTER_H

#include <stdint.h>
#include "lib/utility/ob_print_utils.h"
#include "runtime_utility/common_define.h"

namespace oceanbase
{
namespace transaction
{
class ObTransID;
}
namespace storage
{
namespace mds
{
struct MdsWriter
{
  OB_UNIS_VERSION(1);
public:
  MdsWriter() : writer_type_(WriterType::UNKNOWN_WRITER), writer_id_(INVALID_VALUE) {}
  MdsWriter(const WriterType writer_type, const int64_t writer_id = DEFAULT_WRITER_ID);
  explicit MdsWriter(const transaction::ObTransID &tx_id);
  bool operator==(const MdsWriter &rhs) {
    return writer_type_ == rhs.writer_type_ && writer_id_ == rhs.writer_id_;
  }
  bool is_valid() const;
  TO_STRING_KV("writer_type", obj_to_string(writer_type_), K_(writer_id));
public:
  static constexpr int64_t DEFAULT_WRITER_ID = 10000;

  WriterType writer_type_;
  int64_t writer_id_;
};
OB_SERIALIZE_MEMBER_TEMP(inline, MdsWriter, writer_type_, writer_id_);
}
}
}

#endif