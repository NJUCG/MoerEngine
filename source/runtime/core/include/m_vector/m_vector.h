#ifndef MOER_M_VECTOR_H
#define MOER_M_VECTOR_H

#include <cstddef>
#include <assert.h>
#include <initializer_list>
#include <iterator>
#include <mimalloc.h>

//allocate & construct is divided by the allocator
template<typename T, typename Allocator = mi_stl_allocator<T>>
class m_vector {
public:
    typedef T*             iterator;
    typedef const T*       const_iterator;
    typedef T              value_type;
    typedef std::size_t    size_type;
    typedef std::ptrdiff_t difference_type;

    m_vector() : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {}

    m_vector(size_t n, const T& value = T()) : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {
        reserve(n);
        for (int i = 0; i < n; ++i)
            push_back(value);
    }

    m_vector(iterator _begin, iterator _end) : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {
        reserve(_end - _begin);
        while (_begin != _end) {
            push_back(*_begin);
            ++_begin;
        }
    }

    m_vector(const_iterator _cbegin, const_iterator _cend) : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {
        iterator beg = const_cast<iterator>(_cbegin), end = const_cast<iterator>(_cend);
        reserve(end - beg);
        while (beg != end) {
            push_back(*beg);
            ++beg;
        }
    }

    m_vector(const m_vector& rhs) : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {
        m_vector<T, Allocator> temp(rhs.begin(), rhs.end());
        swap(temp);
    }

    m_vector(m_vector&& rhs) : _start(std::move(rhs._start)), _finish(std::move(rhs._finish)), _endOfStorage(std::move(rhs._endOfStorage)), allocator(std::move(rhs.allocator)) {
        rhs._start        = nullptr;
        rhs._finish       = nullptr;
        rhs._endOfStorage = nullptr;
    }

    //receiving initializer_list as input
    m_vector(const std::initializer_list<T>& list) : _start(nullptr), _finish(nullptr), _endOfStorage(nullptr) {
        reserve(list.size());
        iterator pos = _start;
        for (auto l : list)
            allocator.construct(pos++, l);
        _finish = pos;
    }

    ~m_vector() {
        for (iterator p = _start; p != _finish; ++p) {
            allocator.destroy(p);// p->~T
        }
        allocator.deallocate(_start, capacity());//release the memory
        _start = _finish = _endOfStorage = nullptr;
    }

    //-----------------------------------------

    size_t size() const {
        return _finish - _start;
    }

    size_t capacity() const {
        return _endOfStorage - _start;
    }

    const_iterator cbegin() const {
        const_iterator p = _start;
        return p;
    }

    const_iterator cend() const {
        const_iterator p = _finish;
        return p;
    }

    iterator begin() {
        return _start;
    }

    iterator end() {
        return _finish;
    }

    const_iterator begin() const {
        const_iterator p = _start;
        return p;
    }

    const_iterator end() const {
        const_iterator p = _finish;
        return p;
    }

    iterator data() {
        return _start;
    }

    const_iterator data() const {
        const_iterator beg = _start;
        return beg;
    }

    T& back() {
        iterator pos = _finish - 1;
        return *pos;
    }

    const T& back() const {
        iterator pos = _finish - 1;
        return *pos;
    }

    T& front() {
        return *_start;
    }

    const T& front() const {
        return *_start;
    }

    //-----------------------------------------

    void push_back(const T& value) {
        if (_finish == _endOfStorage) {
            int new_cap = capacity() == 0 ? 10 : 2 * capacity();
            reserve(new_cap);
        }
        allocator.construct(_finish, value);
        ++_finish;
    }

    void push_back(T&& value) {
        if (_finish == _endOfStorage) {
            int new_cap = capacity() == 0 ? 10 : 2 * capacity();
            reserve(new_cap);
        }
        allocator.construct(_finish, std::move(value));
        ++_finish;
    }

    template<class... Args>
    void emplace_back(Args&&... args) {
        if (_finish == _endOfStorage) {
            int new_cap = capacity() == 0 ? 10 : 2 * capacity();
            reserve(new_cap);
        }
        allocator.construct(_finish, std::forward<Args>(args)...);//std::forward: perfect forwarding
        ++_finish;
    }

    void pop_back() {
        assert(!empty() && "vector should not be empty when popping-back");
        allocator.destroy(_finish);
        --_finish;
    }

    //insert before pos
    iterator insert(iterator pos, const T& value) {
        if(_start != _finish)
            assert(pos > _start && pos <= _finish && "out of boundary");
        else
            assert(pos == _start && "invalid pos");

        if (_finish == _endOfStorage) {
            int n   = pos - _start;
            int sum = capacity() == 0 ? 10 : 2 * capacity();
            reserve(sum);
            pos = _start + n;
        }
        iterator p = _finish;
        while (p > pos) {
            *p = *(p - 1);
            --p;
        }
        allocator.construct(p, value);
        ++_finish;
        return pos;
    }

    //insert before pos (multiple values)
    iterator insert(iterator pos, size_t size, const T& value) {
        if (size == 0)
            return pos;
        
        if (_start != _finish)
            assert(pos > _start && pos <= _finish && "out of boundary");
        else
            assert(pos == _start && "invalid pos");

        if (_finish + size > _endOfStorage) {
            int n   = pos - _start;
            int sum = capacity() == 0 ? size + 1 : capacity() + size + 1;// + size + 1 instead of * 2
            reserve(sum);
            pos = _start + n;
        }
        iterator p = _finish + size - 1;
        while (p > pos + size - 1) {
            *p = *(p - size);
            --p;
        }
        while (p >= pos) {
            allocator.construct(p, value);
            --p;
        }
        _finish += size;
        return pos;
    }

    iterator insert(iterator pos, iterator beg, iterator end) {
        if (beg >= end)
            return pos;
        assert(beg && end && "null pointer");

        if (size() != 0)
            assert(pos > _start && pos <= _finish && "out of boundary");
        else
            assert(pos == _start && "invalid pos");

        auto size = end - beg;
        if (_finish + size > _endOfStorage) {
            int n   = pos - _start;
            int sum = capacity() == 0 ? size + 1 : capacity() + size + 1;// + size + 1 instead of * 2
            reserve(sum);
            pos = _start + n;
        }
        iterator p = _finish + size - 1;
        while (p > pos + size - 1) {
            *p = *(p - size);
            --p;
        }
        while (p >= pos) {
            --end;
            allocator.construct(p, *end);
            --p;
        }
        _finish += size;
        return pos;
    }

    iterator erase(iterator pos) {
        assert(_start != _finish && "vector is empty");
        assert(pos >= _start && pos < _finish && "out of boundary");
        allocator.destroy(pos);
        iterator p = pos + 1;
        while (p < _finish) {
            *(p - 1) = *p;
            ++p;
        }
        --_finish;
        return pos;
    }

    iterator erase(iterator beg, iterator end) {
        assert(_finish > _start && "vector is empty");
        assert(beg >= _start && end <= _finish && "out of boundary");
        assert(beg <= end && "begin should less-equal than end");
        for (iterator tmp = beg; tmp < end; ++tmp)
            allocator.destroy(tmp);
        auto pos = beg;
        while (end < _finish) {
            *pos = *end;
            ++pos;
            ++end;
        }
        _finish = pos;
        return beg;
    }

    //delete elements, but not the allocated memory
    void clear() {
        for (iterator p = _start; p != _finish; ++p) {
            if (p)
                allocator.destroy(p);
        }
        _finish = _start;
    }

    void resize(size_t n, const T& value = T()) {
        if (n > capacity())
            reserve(n);
        if (n > size()) {
            while (_finish != _start + n) {
                allocator.construct(_finish, value);//std::move to be continued
                ++_finish;
            }
        } else {
            for (iterator pos = _start + n; pos != _finish; ++pos)
                allocator.destroy(pos);
            _finish = _start + n;
        }
    }

    void reserve(size_t n) {
        if (n > capacity()) {
            T*     p   = allocator.allocate(n);
            T*     pos = p;
            size_t s   = size();
            if (_start) {
                for (int i = 0; i < s; ++i)
                    allocator.construct(pos++, std::move(_start[i]));//must be std::move since classes like unique_ptr can't be copied
                clear();
                allocator.deallocate(_start, capacity());
            }
            _start        = p;
            _finish       = p + s;
            _endOfStorage = _start + n;
        }
    }

    bool empty() const {
        return _finish == _start;
    }

    void swap(m_vector& v) {
        std::swap(_start, v._start);
        std::swap(_finish, v._finish);
        std::swap(_endOfStorage, v._endOfStorage);
    }

    void shrink_to_fit() {
        m_vector tmp(*this);//copy construct has no extra capacity
        swap(tmp);
    }

    //-----------------------------------------

    m_vector& operator=(const m_vector& v) {
        m_vector tmp(v.begin(), v.end());
        swap(tmp);
        return *this;
    }

    m_vector& operator=(m_vector&& v) {
        clear();
        allocator.deallocate(_start, capacity());

        _start        = std::move(v._start);
        _finish       = std::move(v._finish);
        _endOfStorage = std::move(v._endOfStorage);
        allocator     = std::move(v.allocator);

        v._start = v._finish = v._endOfStorage = nullptr;
        return *this;
    }

    m_vector& operator=(std::initializer_list<T> list) {
        m_vector<T, Allocator> tmp(list);
        swap(tmp);
        return *this;
    }

    T& operator[](size_t pos) {
        assert(pos < size() && "out of boundary");
        return _start[pos];
    }

    const T& operator[](size_t pos) const {
        assert(pos < size() && "out of boundary");
        return _start[pos];
    }

    bool operator==(const m_vector& rhs) const {
        if (size() != rhs.size())
            return false;
        auto pos1 = _start;
        auto pos2 = const_cast<iterator>(rhs.begin());
        while (pos1 != _finish) {
            if (*pos1 != *pos2)
                return false;
            ++pos1;
            ++pos2;
        }
        return true;
    }

private:
    Allocator allocator;
    iterator  _start;       //begin
    iterator  _finish;      //end
    iterator  _endOfStorage;//capacity
};

#endif