#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "mem/module.h"

namespace cg::mem {

// Builds the shortest IDA-style signature, starting at `address`, that matches exactly once in the
// module's executable sections. RIP-relative displacements, relative branch targets and immediates
// that look like addresses are wildcarded so the signature survives relinking.
// Returns nullopt if not unique within maxBytes.
std::optional<std::string> GenerateSignature(uintptr_t address, const Module& m, size_t maxBytes = 96);

}  // namespace cg::mem
