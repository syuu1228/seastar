/*
 * lifo-alloc.hh
 *
 *  Created on: Aug 21, 2014
 *      Author: avi
 */

#ifndef LIFO_ALLOC_HH_
#define LIFO_ALLOC_HH_

#include <stack>
#include <memory>


// CRTP
template <class T>
class lifo_allocator {
    static std::stack<void*> _freelist;
public:
    void* operator new(size_t size) {
        if (_freelist.empty()) {
            return ::operator new(size);
        } else {
            auto ret = _freelist.top();
            _freelist.pop();
            return ret;
        }
    }
    void operator delete(void* p) {
        _freelist.push(p);
    }
};

template <class T>
std::stack<void*> lifo_allocator<T>::_freelist;

#endif /* LIFO_ALLOC_HH_ */
