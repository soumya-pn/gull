/*
  modified from the fixed block allocator implementation from Mark
*/

#include <stddef.h>

#include <assert.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "shelf_usage/smart_shelf.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/nvmm_libpmem.h"
#include "common/common.h"
#include "shelf_usage/stack.h"

#include "shelf_usage/fixed_block_allocator.h"

namespace nvmm {


/***************************************************************************/
/*                                                                         */
/* Shelf layout                                                            */
/*                                                                         */
/***************************************************************************/

/*
 * Shelf layout is as follows:
 * 
 *   struct _Shelf_metadata [cache line aligned]
 *   struct _FBA_metadata  [1 cache line]
 *   user metadata         [cache line aligned]
 *   blocks                [size pool_size, block_size aligned]
 * 
 * blocks start at offset first_block.
 */

// This needs to fit in a cache line (see FBA_METADATA_SIZE)
struct _FBA_metadata {
    /*
     * Shelf parameters.  Never changed once initialized.
     */

    uint64_t block_size;  // always a multiple of cache line size
    uint64_t first_block; // offset to first block, always a multiple of block_size

    /*
     * Offset to first never allocated block or 0 if no blocks have
     * been allocated yet.  Never decreases so no ABA counter needed.
     */
    Offset never_allocated;

    Stack first_free;  // stack of the free blocks
};



/***************************************************************************/
/*                                                                         */
/* Utility routines                                                        */
/*                                                                         */
/***************************************************************************/

static inline
uint64_t cas_u64(uint64_t* target, uint64_t old_value, uint64_t new_value) {
    return fam_atomic_u64_compare_and_store(target, old_value, new_value);
}



/***************************************************************************/
/*                                                                         */
/* Constructing and destroying                                             */
/*                                                                         */
/***************************************************************************/

static void throw_incompatible(const char* message_prefix, const char* thing, 
                               const std::string pathname, 
                               uint64_t desired_value, uint64_t actual_value) {
    std::ostringstream message;
    message << message_prefix << " shelf '" << pathname 
            << "' has existing incompatible " << thing << " ("
            << actual_value << " versus desired " << desired_value << ")\n";
    throw std::runtime_error(message.str());
}



FixedBlockAllocator::FixedBlockAllocator(void *addr, 
                                         size_t block_size,
                                         size_t user_metadata_size,
                                         size_t initial_pool_size, 
                                         size_t max_pool_size) :
    underlying_shelf(addr, max_pool_size)
{
    (void) initial_pool_size;  // <<<>>>
    if (block_size == 0)
        block_size = 1;
    // The smallest unit of sharing for The Machine is 1 cache line:
    block_size         = round_up(block_size,         kCacheLineSize);
    user_metadata_size = round_up(user_metadata_size, kCacheLineSize);

    // add space for metadata, ensure blocks block-size-aligned:
    assert(sizeof(size_t) == sizeof(uint64_t));
    uint64_t first_block         = underlying_shelf.start_ptr();
    uint64_t user_metadata_start = first_block;
    first_block += user_metadata_size;
    first_block  = round_up(first_block, block_size);

    if ((size_t)first_block > max_pool_size) {
        std::ostringstream message;
        message << "FixedBlockAllocator::FixedBlockAllocator: there is insufficient space for requested user metadata" << std::endl; 
        throw std::runtime_error(message.str());
    }


    uint64_t old_size = cas_u64(&underlying_shelf->block_size, 0, block_size);
    if (old_size != 0 && old_size != (uint64_t)block_size) 
        throw_incompatible("FixedBlockAllocator::FixedBlockAllocator:", "block size", "", 
                           old_size, block_size);

    old_size = cas_u64(&underlying_shelf->first_block, 0, first_block);
    if (old_size != 0 && old_size != first_block) 
        throw_incompatible("FixedBlockAllocator::FixedBlockAllocator:", "user metadata size", "", 
                           old_size   - user_metadata_start, 
                           block_size - user_metadata_start);
}
    


/***************************************************************************/
/*                                                                         */
/* Inspectors                                                              */
/*                                                                         */
/***************************************************************************/

size_t FixedBlockAllocator::size() { 
    return underlying_shelf.size(); 
}

size_t FixedBlockAllocator::block_size() {
    return underlying_shelf->block_size;
}

int64_t FixedBlockAllocator::max_blocks() {
    return (underlying_shelf.size() - underlying_shelf->first_block)/block_size();
}


void* FixedBlockAllocator::user_metadata() {
    return underlying_shelf[underlying_shelf.start_ptr()];
}

size_t FixedBlockAllocator::user_metadata_size() {
    return underlying_shelf->first_block - underlying_shelf.start_ptr();
}

SmartShelf_& FixedBlockAllocator::get_underlying_shelf() {
    return underlying_shelf;
}


/***************************************************************************/
/*                                                                         */
/* Allocating blocks                                                       */
/*                                                                         */
/***************************************************************************/

Offset FixedBlockAllocator::alloc() {
    /*
     * First, try and allocate a block off of the linked list of free
     * blocks.
     */
    Offset result = underlying_shelf->first_free.pop(underlying_shelf);
    if (result)
        return result;
    
    /*
     * Second, try and allocate a new, never before allocated block.
     */
    // would a non-atomic read be faster here?
    Offset old_never = fam_atomic_u64_read(&underlying_shelf->never_allocated);
    for (;;) {
        Offset block = old_never;
        if (block == 0)
            block = underlying_shelf->first_block;
        Offset new_never = block + underlying_shelf->block_size;
        if ((size_t)new_never > underlying_shelf.size())
            break;

        result = fam_atomic_u64_compare_and_store(&underlying_shelf->never_allocated,
                                                                   old_never, new_never);
        if (result == old_never)
            return block;

        old_never = result;
    }


    return 0;
}



/***************************************************************************/
/*                                                                         */
/* Freeing blocks                                                          */
/*                                                                         */
/***************************************************************************/

void FixedBlockAllocator::free(Offset block) {
    if (block == 0)
        return;

    uint64_t* b = (uint64_t*) underlying_shelf[block];
    pmem_persist(b, underlying_shelf->block_size);

    unsafe_free(block);
}


void FixedBlockAllocator::unsafe_free(Offset block) {
    if (block == 0)
        return;

    underlying_shelf->first_free.push(underlying_shelf, block);
}

}
