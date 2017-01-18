/*
  modified from the shelf implementation from Mark
 */

#ifndef _NVMM_SMART_SHELF_H_
#define _NVMM_SMART_SHELF_H_

#include <stddef.h>
#include <stdint.h>
#include <stdexcept>

#include "nvmm/global_ptr.h"
#include "common/common.h"

namespace nvmm {


class IOError : public std::runtime_error {
public:
    IOError(int error_no);
    int error_no;           // errno value
};


class SmartShelf_ {
public:
    // may throw IOError, (on bad parameters) std::logic_error, or
    // std::runtime error (existing file incompatible with passed parameters)
    SmartShelf_(void *addr, size_t fixed_section_size, size_t max_shelf_size);
    ~SmartShelf_();
    
    /*
     * Valid Offset's for the variable section of this Shelf are 0
     * (NULL equivalent) and start_ptr() .. size()-1.
     */
    Offset start_ptr();  // cacheline aligned
    size_t   size();

    // get start of the fixed section
    void* fixed_section();

    // converting to and from local-address-space pointers
    void*    from_Offset(Offset p); 
    Offset to_Offset  (void*    p);

    // shortcut for from_Offset; does not work well on (Shelf)*:
    //   need (*shelf)[ptr] for that case
    void* operator[](Offset p) { return from_Offset(p); }

    // Shelf_'s are not copyable or assignable:
    SmartShelf_(const SmartShelf_&)            = delete;
    SmartShelf_& operator=(const SmartShelf_&) = delete;

private:
    size_t start;
    void* shelf_location;
    size_t mapped_size;
};


template <typename Fixed>
class SmartShelf : public SmartShelf_ {
public:
    // throws same types as SmartShelf_
    SmartShelf(void *addr, size_t max_shelf_size) :
        SmartShelf_(addr, sizeof(Fixed), max_shelf_size)
        {}

    // provide easy access to fixed section by pretending we are a
    // pointer to it:
    Fixed& operator*()  { return *(Fixed*)fixed_section(); }
    Fixed* operator->() { return  (Fixed*)fixed_section(); }
};

// Specialize with type void if you want no fixed section:
template <>
class SmartShelf<void> : public SmartShelf_ {
public:
    SmartShelf(void *addr, size_t max_shelf_size) :
        SmartShelf_(addr, 0, max_shelf_size)
        {}
};


/***************************************************************************/
/*                                                                         */
/* Implementation-only details follow                                      */
/*                                                                         */
/***************************************************************************/

inline void* SmartShelf_::fixed_section() {
    return (char*)shelf_location+kCacheLineSize;
}

inline void* SmartShelf_::from_Offset(Offset p) {
    if (p == 0)
        return NULL;
    else
        return (char*)shelf_location + p;
}

inline Offset SmartShelf_::to_Offset(void* p) {
    if (p == NULL)
        return 0;
    else
        return (char*)p - (char*)shelf_location;
}


}

#endif
