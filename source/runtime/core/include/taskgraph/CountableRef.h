#ifndef COUNTABLE_REF_H
#define COUNTABLE_REF_H
#include <atomic>
#include <assert.h>
#include <type_traits>
class Countable{
public:
	int32_t AddRef() {
		return m_counter.fetch_add(1)+1;
	};
	virtual void Destroy() = 0;
	int32_t DeRef() {
		assert(m_counter >= 0);
		int32_t current = m_counter.fetch_sub(1);
		if (current == 1) { 
			Destroy();
		}
		return current - 1;
	};
	int32_t GetRefCount() { return m_counter.load(); }
	
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
	CountableRef(T* countable, bool AddRef = true) {
		ptr = countable;
		if (ptr != nullptr && AddRef) {
			ptr->AddRef();
		}
	}

	CountableRef(const CountableRef& copy) {
		ptr = copy.ptr;
		if (ptr != nullptr) {
			ptr->AddRef();
		}
	}
	template<typename CopyType>
	CountableRef(const CountableRef<CopyType>& copy) {
		ptr = copy.ptr;
		if (ptr != nullptr) {
			ptr->AddRef();
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
			ptr->DeRef();
		}
	}
	CountableRef& operator=(T* _ptr) {
		if (ptr != _ptr) {
			T* old = ptr;
			ptr = _ptr;
			if (ptr != nullptr) {
				ptr->AddRef();
			}
			if (old != nullptr) {
				old->DeRef();
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
				old->DeRef();
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
				old->DeRef();
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
	int32_t GetRefCount() {
		return ptr->GetRefCount();
	}
protected:
	T* ptr;
};
#endif // !COUNTABLE_REF_H
