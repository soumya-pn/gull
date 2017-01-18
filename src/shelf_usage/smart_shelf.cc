/*
  modified from the shelf implementation from Mark
 */

#include <stddef.h>
#include <stdint.h>
#include <stdexcept>

#include <iostream>
#include <assert.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "nvmm/global_ptr.h"
#include "common/common.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "common/common.h"

#include "shelf_usage/smart_shelf.h"

namespace nvmm {


/***************************************************************************/
/*                                                                         */
/* Shelf layout                                                            */
/*                                                                         */
/***************************************************************************/

/*
 * LFS Shelf layout:
 *   struct Shelf_metadata   [1 cache line]
 *   fixed section           [multiple of cache lines]
 *   variable section
 */

// This needs to fit in a cache line (see fixed_section())
struct Shelf_metadata {
    /*
     * Shelf parameters.  Never changed once initialized.
     */
    int64_t fixed_section_size;
    int64_t max_shelf_size;
};



/***************************************************************************/
/*                                                                         */
/* IOError                                                                 */
/*                                                                         */
/***************************************************************************/

static std::string strerror_as_string(int error) {
    char buffer[1000];
    // we get GNU version in spite of what man page says:
#if 1
    char* result = strerror_r(error, buffer, sizeof(buffer));
#else
    int status = strerror_r(error, buffer, sizeof(buffer));
    assert(status == 0);
    char* result = buffer;
#endif
    return std::string(result);
}

IOError::IOError(int error_no) : 
    runtime_error(std::string("I/O error: ") + strerror_as_string(error_no)), 
    error_no(error_no) 
{};



/***************************************************************************/
/*                                                                         */
/* Utility routines                                                        */
/*                                                                         */
/***************************************************************************/

static inline
int64_t cas64(int64_t* target, int64_t old_value, int64_t new_value) {
    return fam_atomic_64_compare_and_store(target, old_value, new_value);
}



/***************************************************************************/
/*                                                                         */
/* SmartShelf_                                                                  */
/*                                                                         */
/***************************************************************************/
SmartShelf_::SmartShelf_(void *addr, size_t fixed_section_size, 
               size_t max_shelf_size) {
    max_shelf_size = round_up(max_shelf_size, kVirtualPageSize);
    start = kCacheLineSize + fixed_section_size;
    start = round_up(start, kCacheLineSize);

    if (start > max_shelf_size)
        throw std::logic_error(std::string("Shelf size too small for Shelf_ overhead+fixed section"));

    assert(addr != NULL);
    shelf_location = addr;
    mapped_size = max_shelf_size;

    struct Shelf_metadata* metadata = (struct Shelf_metadata*) shelf_location;
    static_assert( sizeof(Shelf_metadata) <= kCacheLineSize, "Shelf_metadata is too big!");

    int64_t old_size = cas64(&metadata->fixed_section_size, 0, fixed_section_size);
    if (old_size != 0 && old_size != (int64_t)fixed_section_size) {
        std::cout << "old_size " << old_size << " fixed_size " << fixed_section_size << std::endl;
        throw std::runtime_error("Shelf has different fixed section size from one specified");
    }

    old_size = cas64(&metadata->max_shelf_size, 0, max_shelf_size);
    if (old_size != 0 && old_size != (int64_t)max_shelf_size) {
        std::cout << "old_size " << old_size << " max_size " << max_shelf_size << std::endl;
        throw std::runtime_error("Shelf has different maximum size from one specified");
    }
}
                   
SmartShelf_::~SmartShelf_()
{
}

Offset SmartShelf_::start_ptr() {
    return start;
}

size_t SmartShelf_::size() {
    return mapped_size;
}

}
