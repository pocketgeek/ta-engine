#pragma once

#include <memory>
#include <optional>
#include <utility>

namespace tak::sim {

// A std::optional whose payload lives on the heap: the member itself is one
// pointer, so a large, rarely engaged payload stops pushing a record's hot
// fields onto further cache lines, and its presence test reads 8 bytes inside
// the record. Value semantics, like std::optional: copying deep-copies the
// payload, comparison compares values. The only difference is that a moved-
// from HeapOptional is empty (a moved-from std::optional stays engaged with a
// moved-from value); nothing reads an Order after moving it away.
template<class T>
class HeapOptional {
public:
    using value_type = T;
    HeapOptional() = default;
    HeapOptional(std::nullopt_t) {}
    HeapOptional(const T& v) : p_(std::make_unique<T>(v)) {}
    HeapOptional(T&& v) : p_(std::make_unique<T>(std::move(v))) {}
    HeapOptional(const std::optional<T>& v) { if (v) p_ = std::make_unique<T>(*v); }
    HeapOptional(const HeapOptional& o) { if (o.p_) p_ = std::make_unique<T>(*o.p_); }
    HeapOptional(HeapOptional&&) noexcept = default;
    HeapOptional& operator=(const HeapOptional& o) {
        if (this == &o) return *this;
        if (!o.p_) p_.reset();
        else if (p_) *p_ = *o.p_;
        else p_ = std::make_unique<T>(*o.p_);
        return *this;
    }
    HeapOptional& operator=(HeapOptional&&) noexcept = default;
    HeapOptional& operator=(std::nullopt_t) { p_.reset(); return *this; }
    HeapOptional& operator=(const T& v) {
        if (p_) *p_ = v; else p_ = std::make_unique<T>(v);
        return *this;
    }
    HeapOptional& operator=(T&& v) {
        if (p_) *p_ = std::move(v); else p_ = std::make_unique<T>(std::move(v));
        return *this;
    }
    HeapOptional& operator=(const std::optional<T>& v) {
        if (v) *this = *v; else p_.reset();
        return *this;
    }
    operator std::optional<T>() const { return p_ ? std::optional<T>(*p_) : std::nullopt; }

    template<class... A> T& emplace(A&&... a) {
        p_ = std::make_unique<T>(std::forward<A>(a)...);
        return *p_;
    }
    void reset() { p_.reset(); }
    bool has_value() const { return p_ != nullptr; }
    explicit operator bool() const { return p_ != nullptr; }
    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    const T* operator->() const { return p_.get(); }
    T& value() { if (!p_) throw std::bad_optional_access(); return *p_; }
    const T& value() const { if (!p_) throw std::bad_optional_access(); return *p_; }
    template<class U> T value_or(U&& d) const { return p_ ? *p_ : T(std::forward<U>(d)); }

    friend bool operator==(const HeapOptional& a, const HeapOptional& b) {
        return a.p_ ? (b.p_ && *a.p_ == *b.p_) : !b.p_;
    }
    friend bool operator==(const HeapOptional& a, std::nullopt_t) { return !a.p_; }

private:
    std::unique_ptr<T> p_;
};

} // namespace tak::sim
