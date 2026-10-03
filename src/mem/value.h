#pragma once
// Value types shared by the memory scanner, address table, struct dissector and scripts.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cg::mem {

enum class ValueType : uint8_t { I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, String, WString, Bytes };

inline constexpr ValueType kAllValueTypes[] = {ValueType::I8,  ValueType::I16, ValueType::I32,    ValueType::I64,    ValueType::U8,
                                               ValueType::U16, ValueType::U32, ValueType::U64,    ValueType::F32,    ValueType::F64,
                                               ValueType::String, ValueType::WString, ValueType::Bytes};

const char* ValueTypeName(ValueType t);                       // "i32", "f32", "string", "bytes", ...
std::optional<ValueType> ParseValueType(std::string_view s);  // inverse of ValueTypeName (case-insensitive)
size_t ValueSize(ValueType t);                                // fixed size; 0 for String/WString/Bytes
bool IsNumeric(ValueType t);
bool IsFloat(ValueType t);

// Text <-> bytes. Numbers accept decimal or 0x-hex; Bytes accept "90 90 ??"-free hex pairs;
// String is UTF-8 (no NUL appended), WString is UTF-16LE.
bool ParseValue(ValueType t, std::string_view text, std::vector<uint8_t>& out);
std::string FormatValue(ValueType t, const void* data, size_t size, bool hex = false);

// Reads and formats live memory ("??" when unreadable). For String/WString/Bytes uses `len`.
std::string ReadFormatted(uintptr_t addr, ValueType t, size_t len = 16, bool hex = false);

}  // namespace cg::mem
