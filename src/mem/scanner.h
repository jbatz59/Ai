#pragma once
// Cheat-Engine-style value scanner over the process' committed memory. Runs on worker threads
// (all cores); the UI polls Busy()/Progress() and pages through results.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mem/value.h"

namespace cg::mem {

enum class ScanOp : uint8_t {
  Exact, NotEqual, Bigger, Smaller, Between,      // need value (Between: value..value2 inclusive)
  Unknown,                                        // first scan only: snapshot everything
  Changed, Unchanged, Increased, Decreased,       // next scans only
  IncreasedBy, DecreasedBy                        // next scans only, need value
};
const char* ScanOpName(ScanOp op);
bool ScanOpNeedsValue(ScanOp op);
bool ScanOpValidForFirst(ScanOp op);

struct ScanParams {
  ValueType type = ValueType::I32;
  ScanOp op = ScanOp::Exact;
  std::string value, value2;
  bool writableOnly = true;       // skip read-only pages (code, constants)
  bool includeImages = false;     // include module images (statics) — default heap/stack only
  bool fastScan = true;           // only aligned addresses (alignment = min(size, 4))
  bool floatRounded = true;       // F32/F64 Exact: compare with tolerance derived from the typed decimals
  uintptr_t rangeStart = 0x10000;
  uintptr_t rangeEnd = 0x7FFFFFFF0000ull;
};

class Scanner {
 public:
  Scanner();
  ~Scanner();

  // Both return false if busy or params invalid (error in LastError()). Work continues async.
  bool FirstScan(const ScanParams& p);
  bool NextScan(const ScanParams& p);   // type must match the first scan
  void Reset();
  void Cancel();

  bool Busy() const;
  float Progress() const;               // 0..1
  bool HasResults() const;
  size_t ResultCount() const;
  std::string LastError() const;
  ValueType Type() const;
  uint64_t Generation() const;          // bumps when results change

  struct Row {
    uintptr_t address;
    std::string current;   // re-read live
    std::string previous;  // value captured by the last scan
  };
  std::vector<Row> Page(size_t offset, size_t count, bool hex = false) const;

  // Unknown-initial-value snapshots are capped (default 2 GiB) to protect the game's memory.
  void SetSnapshotLimit(uint64_t bytes);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cg::mem
