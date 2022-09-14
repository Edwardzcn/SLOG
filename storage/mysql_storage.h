#pragma once

#include "data_structure/mysql_database.h"
#include "storage/lookup_master_index.h"
#include "storage/storage.h"

namespace slog {
class MySQLStorage : public Storage, public LookupMasterIndex {
 public:
  bool Read(const Key& key, Record& result) const final { return db_.Get(result, key); }

  bool Write(const Key& key, const Record& record) final { return db_.InsertOrUpdate(key, record); }

  bool Delete(const Key& key) final { return db_.Erase(key); }

  bool GetMasterMetadata(const Key& key, Metadata& metadata) const final {
    Record rec;
    if (!db_.Get(rec, key)) {
      return false;
    }
    metadata = rec.metadata();
    return true;
  }

 private:
  /* data */
  MySQLDatabase<Key, Record> db_;
};

}  // namespace slog