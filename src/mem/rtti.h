#pragma once
// MSVC x64 RTTI discovery. Mafia: DE is built with MSVC; if the binary keeps RTTI, every
// polymorphic class (e.g. "C_Player2") can be located by *name* at runtime — no byte patterns,
// survives game patches. Layouts (x64, image-relative):
//   TypeDescriptor            { void* pVFTable; void* spare; char name[]; }          name = ".?AVC_Foo@@"
//   CompleteObjectLocator     { u32 signature(=1); u32 offset; u32 cdOffset; i32 pTypeDescriptor;
//                               i32 pClassDescriptor; i32 pSelf; }
//   ClassHierarchyDescriptor  { u32 signature; u32 attributes; u32 numBaseClasses; i32 pBaseClassArray; }
//   BaseClassDescriptor       { i32 pTypeDescriptor; u32 numContainedBases; i32 mdisp, pdisp, vdisp;
//                               u32 attributes; i32 pClassDescriptor; }
//   vtable[-1] = &CompleteObjectLocator
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mem/module.h"

namespace cg::mem::rtti {

struct VTableInfo {
  uintptr_t address = 0;    // first virtual function slot
  uint32_t offset = 0;      // COL.offset: subobject offset inside the complete object (0 = primary)
  size_t functionCount = 0; // slots pointing into executable memory
};

struct ClassInfo {
  std::string mangled;              // ".?AVC_Player2@@"
  std::string name;                 // "C_Player2" (namespaces as "ns::C")
  uintptr_t typeDescriptor = 0;
  std::vector<VTableInfo> vtables;  // primary (offset 0) first
  std::vector<std::string> bases;   // demangled base class names, nearest first, excluding self
};

// Builds (once, cached per module base) the index of every RTTI class. Thread-safe. Typically
// 100-500 ms on a large game binary; call from a worker thread.
const std::vector<ClassInfo>& Index(const Module& m);
void InvalidateIndex();

const ClassInfo* FindClass(const Module& m, std::string_view nameOrMangled);   // exact, case-sensitive
std::optional<uintptr_t> PrimaryVTable(const Module& m, std::string_view name);

// Class of a live object (reads *object -> vtable -> COL -> TypeDescriptor). "" if not polymorphic.
std::string ClassNameOf(uintptr_t object);
bool IsA(uintptr_t object, std::string_view className);   // exact class or any base

std::string Demangle(std::string_view mangled);           // ".?AVC_Foo@ns@@" -> "ns::C_Foo"

// Scans committed private read/write memory for objects whose first qword == vtable. Expensive
// (seconds); call from a worker thread; honours `cancel`; `progress` gets 0..1.
std::vector<uintptr_t> FindInstances(uintptr_t vtable, size_t maxResults = 256, const std::atomic<bool>* cancel = nullptr,
                                     std::atomic<float>* progress = nullptr);

}  // namespace cg::mem::rtti
