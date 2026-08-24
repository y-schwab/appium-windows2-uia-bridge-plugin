#pragma once
// Minimal COM smart pointer — avoids pulling in ATL (<atlbase.h>) just for CComPtr in a project
// that otherwise has no ATL dependency.

#include <unknwn.h>
#include <utility>

namespace UiaBridge {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    explicit ComPtr(T* p) : ptr_(p) {}
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) { if (ptr_) { ptr_->AddRef(); } }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

    ~ComPtr() { Reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            if (ptr_) { ptr_->AddRef(); }
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    // Takes ownership of an already-AddRef'd pointer (e.g. straight out of QueryInterface).
    void Attach(T* p) {
        Reset();
        ptr_ = p;
    }

    T* Detach() {
        T* p = ptr_;
        ptr_ = nullptr;
        return p;
    }

    T** ReleaseAndGetAddressOf() {
        Reset();
        return &ptr_;
    }

    T* Get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

} // namespace UiaBridge
