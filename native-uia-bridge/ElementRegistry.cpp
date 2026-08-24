#include "ElementRegistry.h"

namespace UiaBridge {

std::wstring ElementRegistry::Register(const AccessibleRef& ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring id = L"e" + std::to_wstring(nextId_++);
    byId_[id] = ref;
    return id;
}

void ElementRegistry::RegisterRoot(const AccessibleRef& ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    byId_[kRootInternalId] = ref;
}

bool ElementRegistry::TryGet(const std::wstring& internalId, AccessibleRef& outRef) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byId_.find(internalId);
    if (it == byId_.end()) {
        return false;
    }
    outRef = it->second;
    return true;
}

void ElementRegistry::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    byId_.clear();
}

} // namespace UiaBridge
