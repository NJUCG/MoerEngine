#ifndef COUNTABLE_REF_H
#define COUNTABLE_REF_H
#include <atomic>
#include <assert.h>
#include <type_traits>
#include "spdlog/spdlog.h"
class Countable{
public:
	int32_t addRef() {
		return m_counter.fetch_add(1)+1;
	};
	virtual void destroy() = 0;
	int32_t tryDrop() {
		assert(m_counter >= 0);
		int32_t current = m_counter.fetch_sub(1);
		if (current == 1) { 
			destroy();
		}
		return current - 1;
	};
	int32_t getRefCount() { return m_counter.load(); }
	
protected:
	std::atomic<int32_t> m_counter;
};

template<typename T>
class CountableRef {
public:
	CountableRef() :ptr{ nullptr } { 
		//if(std::is_convertible<T, Countable>::value) return;
		//assert(false);
	}
	CountableRef(T* countable, bool addRef = true) {
		ptr = countable;
		if (ptr != nullptr && addRef) {
			ptr->addRef();
		}
	}

	CountableRef(const CountableRef& copy) {
		ptr = copy.ptr;
		if (ptr != nullptr) {
			ptr->addRef();
		}
	}
	template<typename CopyType>
	CountableRef(const CountableRef<CopyType>& copy) {
		ptr = copy.ptr;
		if (ptr != nullptr) {
			ptr->addRef();
		}
	}
	template<typename MoveType>
	CountableRef(CountableRef<MoveType>&& move) {
		ptr = move.ptr;
		move.ptr = nullptr;
	}

	CountableRef(CountableRef&& move) {
		ptr = move.ptr;
		move.ptr = nullptr;
	}
	~CountableRef() {
		if (ptr != nullptr) {
			ptr->tryDrop();
		}
	}
	CountableRef& operator=(T* _ptr) {
		if (ptr != _ptr) {
			T* old = ptr;
			ptr = _ptr;
			if (ptr != nullptr) {
				ptr->addRef();
			}
			if (old != nullptr) {
				old->tryDrop();
			}
		}
		return *this;
	}
	template<typename RefType>
	CountableRef& operator=(const CountableRef<RefType>& _ref) {
		return *this = _ref.ptr;
	}
	CountableRef& operator=(const CountableRef& _ref) {
		return *this = _ref.ptr;
	}

	template<typename MoveType>
	CountableRef& operator=(CountableRef<MoveType>&& _ref_move) {
		if (this != &_ref_move) {
			Countable* old = ptr;
			ptr = _ref_move.ptr;
			_ref_move.ptr = nullptr;
			if (old != nullptr) {
				old->tryDrop();
			}
		}
		return *this;
	}

	CountableRef& operator=(CountableRef&& _ref_move) {
		if (this != &_ref_move) {
			Countable* old = ptr;
			ptr = _ref_move.ptr;
			_ref_move.ptr = nullptr;
			if (old != nullptr) {
				old->tryDrop();
			}
		}
		return *this;
	}
	T* get() const{ return ptr; }
	T* operator->() const{
		return ptr;
	}
	operator T() const {
		return ptr;
	}
	bool isValid() const{
		return ptr != nullptr;
	}

	void swap(CountableRef& other) {
		T* old = ptr;
		ptr = other.ptr;
		other.ptr = old;
	}
	template<typename Other>
	inline bool operator==(const CountableRef<Other>& other) {
		return ptr == other.ptr;
	}

	inline bool operator==(const CountableRef& other) {
		return ptr == other.ptr;
	}

	inline bool operator==(const T* other) {
		return ptr == other;
	}
	int32_t getRefCount() {
		return ptr->getRefCount();
	}
protected:
	T* ptr;
};
#endif // !COUNTABLE_REF_H
