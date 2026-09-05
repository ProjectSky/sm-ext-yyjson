#pragma once

#include <cassert>
#include <cstddef>
#include <utility>

/**
 * @brief Base class for intrusive reference counting
 *
 * Objects inheriting from this class can be managed by RefPtr.
 * Reference count starts at 0 and is incremented when RefPtr takes ownership.
 */
class RefCounted {
private:
	mutable size_t ref_count_ = 0;

protected:
	virtual ~RefCounted() = default;
	RefCounted() = default;

	RefCounted(const RefCounted &) = delete;
	RefCounted &operator=(const RefCounted &) = delete;

	RefCounted(RefCounted &&) noexcept : ref_count_(0) {}
	RefCounted &operator=(RefCounted &&) noexcept { return *this; }

public:
	void add_ref() const noexcept { ++ref_count_; }

	void release() const noexcept {
		assert(ref_count_ > 0 && "Reference count underflow");
		if (--ref_count_ == 0) {
			delete this;
		}
	}

	size_t use_count() const noexcept { return ref_count_; }
};

/**
 * @brief Smart pointer for intrusive reference counting
 *
 * Similar to std::shared_ptr but uses intrusive reference counting.
 * Automatically manages object lifetime through add_ref() and release().
 * @warning This implementation is not thread-safe. It must only be used
 *          in single-threaded contexts or with external synchronization.
 */
template <typename T> class RefPtr {
private:
	T *ptr_;

public:
	RefPtr() noexcept : ptr_(nullptr) {}

	explicit RefPtr(T *p) noexcept : ptr_(p) {
		if (ptr_)
			ptr_->add_ref();
	}

	RefPtr(const RefPtr &other) noexcept : ptr_(other.ptr_) {
		if (ptr_)
			ptr_->add_ref();
	}

	RefPtr(RefPtr &&other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

	~RefPtr() {
		if (ptr_)
			ptr_->release();
	}

	RefPtr &operator=(const RefPtr &other) noexcept {
		if (ptr_ != other.ptr_) {
			if (other.ptr_)
				other.ptr_->add_ref();
			if (ptr_)
				ptr_->release();
			ptr_ = other.ptr_;
		}
		return *this;
	}

	RefPtr &operator=(RefPtr &&other) noexcept {
		if (this != &other) {
			if (ptr_)
				ptr_->release();
			ptr_ = other.ptr_;
			other.ptr_ = nullptr;
		}
		return *this;
	}

	RefPtr &operator=(T *p) noexcept {
		reset(p);
		return *this;
	}

	T *operator->() const noexcept { return ptr_; }
	T &operator*() const noexcept { return *ptr_; }
	T *get() const noexcept { return ptr_; }

	explicit operator bool() const noexcept { return ptr_ != nullptr; }

	size_t use_count() const noexcept { return ptr_ ? ptr_->use_count() : 0; }

	void reset(T *p = nullptr) noexcept {
		if (ptr_ != p) {
			if (p)
				p->add_ref();
			if (ptr_)
				ptr_->release();
			ptr_ = p;
		}
	}

	bool operator==(const RefPtr &other) const noexcept { return ptr_ == other.ptr_; }
	bool operator!=(const RefPtr &other) const noexcept { return ptr_ != other.ptr_; }
	bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }
	bool operator!=(std::nullptr_t) const noexcept { return ptr_ != nullptr; }
};

/**
 * @brief Factory function to create RefPtr from new object
 */
template <typename T, typename... Args> RefPtr<T> make_ref(Args &&...args) {
	return RefPtr<T>(new T(std::forward<Args>(args)...));
}
