#pragma once
#include <cstring>
#include <new>
extern int allocation_fail_after;
template<class T> T *testNew(size_t count) {
    if(allocation_fail_after==0)return nullptr;
    if(allocation_fail_after>0)--allocation_fail_after;
    return new(std::nothrow) T[count];
}
#define IONewData(T,n) testNew<T>(n)
#define IODeleteData(p,T,n) delete[] (p)
#define bzero(p,n) std::memset((p),0,(n))
