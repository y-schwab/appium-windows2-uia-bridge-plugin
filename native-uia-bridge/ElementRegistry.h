#pragma once
// Maps the opaque string ids used on the JSON-RPC wire (see BridgeServer.cpp) to the underlying
// AccessibleRef (IAccessible* + childId) they refer to. One registry per attached container —
// this bridge supports exactly one F3 Server / Forms 2.0 container per injected process in v1
// (matches the task's single-container scope); injecting into a process a second time is a
// no-op (LoadLibrary just bumps the refcount without re-running DllMain), so multi-container
// coverage within one process is a documented future extension, not a v1 requirement.

#include <mutex>
#include <string>
#include <unordered_map>

#include "AccessibleTree.h"

namespace UiaBridge {

constexpr const wchar_t* kRootInternalId = L"root";

class ElementRegistry {
public:
    // Registers `ref` and returns a fresh internal id. Called every time a node is produced by
    // Navigate/Find — duplicate registrations for the same logical node across multiple calls
    // are expected and harmless (each entry is independently valid), see header comment above.
    std::wstring Register(const AccessibleRef& ref);

    void RegisterRoot(const AccessibleRef& ref);

    bool TryGet(const std::wstring& internalId, AccessibleRef& outRef) const;

    void Clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::wstring, AccessibleRef> byId_;
    long nextId_ = 1;
};

} // namespace UiaBridge
