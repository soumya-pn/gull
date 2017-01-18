#include <stddef.h>
#include <stdint.h>

#include <assert.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <time.h>

#include "nvmm/global_ptr.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/nvmm_libpmem.h"
#include "common/common.h"
#include "shelf_usage/stack.h"

#include "shelf_usage/zone.h"

struct timespec start, end;

namespace nvmm {

inline uint64_t power_of_two(uint64_t n)
{
        uint64_t power = 0;
        if (n == 0) {
                return 0;
        }
        while (n != 1) {
                if (n % 2 != 0) {
                        return 0;
                }
                n = n >> 1;
                power = power + 1;
        }
        return power;
}

inline bool is_power_of_two(uint64_t n)
{
	return ((n != 0) && !(n & (n-1)));
}

inline uint64_t next_power_of_two(uint64_t n)
{
	if (is_power_of_two(n)) {
		return n;
	} else {
		n |= n >> 1;
		n |= n >> 2;
		n |= n >> 4;
		n |= n >> 8;
		n |= n >> 16;
		n |= n >> 32;
		return n + 1;
	}		
}

#define BYTE 8UL
#define KB 1024UL
#define MB (KB * KB)
#define GB (MB * KB)
//#define TB (GB * KB)
#define MAX_ZONE_SIZE (128 * GB)
#define MIN_OBJECT_SIZE 64
#define MAX_LEVEL_PER_ZONE power_of_two(MAX_ZONE_SIZE / MIN_OBJECT_SIZE)
//TODO: Add zoneheader size ????
#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define OFFSET_BITS 48
// TODO: Possibly an enum instead for merge states.
#define MERGE_DEFAULT 0
#define MERGE_SWAP_COMPLETED 1
#define MERGE_BITMAP_COMPLETED 2
#define MERGE_FREELIST_COMPLETED 3

// API to find which level the size belongs to.
inline uint64_t find_level_from_size(uint64_t size, size_t min_obj_size)
{
	// Another way that avoids division:
	// level = power_of_two(size) - power_of_two(min_obj_size).
	return power_of_two(size / min_obj_size);
}

// API to find the size of the object based on the level.
inline uint64_t find_size_from_level(uint64_t level, size_t min_obj_size)
{
        return 1UL << (level + power_of_two(min_obj_size));
}

// API to append the level details to the Offset
inline Offset append_level_to_Offset(Offset ptr, uint64_t level)
{
	return ((level << OFFSET_BITS) | ptr);
}

// API to remove level details from the Offset.
inline Offset remove_level_from_Offset(Offset ptr)
{
	return (~(255UL << OFFSET_BITS) & ptr);
}

// API to get level details from the Offset.
inline uint64_t get_level_from_Offset(Offset ptr)
{
	return (ptr >> OFFSET_BITS);
}

/*
 * Zone Header
 */
struct Zone_Header {
        // The max level of free list allowed in a Zone.
        uint64_t max_zone_level;
        // The max size of the Zone.
        size_t max_zone_size;
        // The multiple factor is used to find the allocation size.
        uint64_t multiple_factor;
        // The mimimum size of the object supported in the Zone
        size_t min_object_size; 
        // Current level of the zone. 
        uint64_t current_zone_level;
        // Starting address of the bitmap.
        Offset bitmap_start_addr;
	// Epoch stored to track the grow and prevent multiple processes from growing simultaneously.
	uint64_t grow_in_progress;
	// Epoch stored to track the merge and prevent multiple processes from merge simultaneously.
	uint64_t merge_in_progress;
	// Track the status of merge so we can recover from crash and restart merge.
	uint64_t merge_status;
	// Starting address used for merge bitmap
	Offset merge_bitmap_start_addr;
	// Current level where merge is happening.
	uint64_t current_merge_level;
	// Stack use to 
	Stack safe_copy;
	// Stack used to track the post merge freelist level.
	Stack post_merge_level;
	// Stack used to track the post merge freelist for level+!.
	Stack post_merge_next_level;
	// Array of stack to track the freelist for various freelists.
	// Note: Never ever directly use the size of Zone_Header directly as it will never include
	// the below array of Stack.
	Stack free_list[0];
};

/***************************************************************************/
/*                                                                         */
/* Utility routines                                                        */
/*                                                                         */
/***************************************************************************/

static inline
int64_t cas64(int64_t* target, int64_t old_value, int64_t new_value) {
    return fam_atomic_64_compare_and_store(target, old_value, new_value);
}


/*
Constructor for the Zone object. We here just pass in the mmap'ed memory
address and max pool size to initialize the underlying Shelf object.
Here, as the application/caller is responsible for the mmap, we dont have to
do anything. This is only called when the init is done. If this is called without
an initialization of the Zone Header, one can notice unexpected behavior and can
lead to corruption.
*/
Zone::Zone(void *addr, 
	   size_t max_pool_size) :
    shelf_location_ptr((char*)addr)
{
	return;
}

/*
Constructor for the Zone object. We here pass the mmap'ed memory,
initial pool size, minimum object size and max pool size to initialize
the underlying Shelf Object. As the application/caller has already done
the mmap, we just have to set the underlying shelf object fields
appropriately and then initialize the Zone Header.
*/
Zone::Zone(void *addr, 
	   size_t initial_pool_size, 
	   size_t min_obj_size,
	   size_t max_pool_size):
    shelf_location_ptr((char*)addr)
{
	uint64_t max_level_per_zone = 0;
	size_t bitmap_size = 0;
	size_t merge_bitmap_size = 0;
	int64_t old_value;
	struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;
	uint64_t zoneheader_size;
	uint64_t freelist_level;
	void *advance_ptr;
	Offset ptr;
	size_t chunk_size, zoneheader_chunk_size;
	std::ostringstream message;

	if (min_obj_size < MIN_OBJECT_SIZE) {
		message << "min size less than " << MIN_OBJECT_SIZE << std::endl;
		throw std::runtime_error(message.str());
	}

	if (!(is_power_of_two(min_obj_size))) {
		message << "min size is not a power of two " << std::endl;
		throw std::runtime_error(message.str());
	}
		

	old_value = cas64((int64_t *)&zoneheader->min_object_size, 0, MAX(MIN_OBJECT_SIZE, min_obj_size));
	if (old_value != 0) {
		message << "Zone header init failed" << std::endl;
		throw std::runtime_error(message.str());
	}

	// TODO: User driven multiple factor???
	
	old_value = cas64((int64_t *)&zoneheader->multiple_factor, 0, 1);
	if (old_value != 0) {
		message << "Zone header init failed" << std::endl;
		throw std::runtime_error(message.str());
	}
	
	if (max_pool_size > MAX_ZONE_SIZE) {
		message << "max_pool_size more than " << MAX_ZONE_SIZE << std::endl;
		throw std::runtime_error(message.str());
	}

	if (!(is_power_of_two(max_pool_size))) {
		message << "max_pool_size is not a power of two" << std::endl;
		throw std::runtime_error(message.str());
	}

	old_value = cas64((int64_t *)&zoneheader->max_zone_size, 0, MIN(max_pool_size, MAX_ZONE_SIZE));
	if (old_value != 0) {
		message << "Zone header init failed" << std::endl;
		throw std::runtime_error(message.str());
	}
	
	max_level_per_zone = find_level_from_size(zoneheader->max_zone_size, zoneheader->min_object_size);
	old_value = cas64((int64_t *)&zoneheader->max_zone_level, 0, MIN(max_level_per_zone, MAX_LEVEL_PER_ZONE));
	if (old_value != 0) {
		message << "Zone header init failed" << std::endl;
		throw std::runtime_error(message.str());
	}

        // zoneheader_size is rounded up to the closest power of two (i.e., a valid object/chunk size)
	zoneheader_size = next_power_of_two(sizeof(Zone_Header) + (sizeof(Stack) * (zoneheader->max_zone_level + 1)));

	// Min value should be 8 bytes.
	bitmap_size = ((1UL << (zoneheader->max_zone_level + 1)) / BYTE);
	merge_bitmap_size = ((1UL << zoneheader->max_zone_level) / BYTE);
	printf("Zone Header size = %ld, Bitmap Size = %ld, Merge Bitmap Size = %ld\n", zoneheader_size, bitmap_size, merge_bitmap_size);

	assert(bitmap_size >= 8 && merge_bitmap_size >= 8);

	if (initial_pool_size <= (bitmap_size + merge_bitmap_size)) {
		message << "initial_pool_size is less than minimum zone size of " << (bitmap_size + merge_bitmap_size) << std::endl;
		throw std::runtime_error(message.str());
	} else if (!(is_power_of_two(initial_pool_size))) {
		message << "initial_pool_size is not a power of two " << std::endl;
		throw std::runtime_error(message.str());
	} else if (initial_pool_size <= zoneheader->min_object_size) {
		message << "initial_pool_size is less than or equal to the minimum object size" << std::endl;
		throw std::runtime_error(message.str());
	}

	old_value = cas64((int64_t *)&zoneheader->current_zone_level, 0,
			 find_level_from_size(initial_pool_size, zoneheader->min_object_size));
	if (old_value != 0) {
		message << "Zone header init failed" << std::endl;
		throw std::runtime_error(message.str());
	}

	// TODO: Zoneheader size > bitmap_size/ merge_bitmap_size
	// i.e the bitmap size is too small compared to zoneheader size.

	// Zoneheader + Bitmap fits in a single min sized chunk.
	if ((zoneheader_size + bitmap_size + merge_bitmap_size) <= zoneheader->min_object_size) {
		advance_ptr = shelf_location_ptr + zoneheader_size;
		old_value = cas64((int64_t *)&zoneheader->merge_bitmap_start_addr, 0,
				 to_Offset(advance_ptr));
		if (old_value != 0) {
			message << "Zone header init failed" << std::endl;
			throw std::runtime_error(message.str());
		}

		advance_ptr = (char*)advance_ptr + merge_bitmap_size;
		old_value = cas64((int64_t *)&zoneheader->bitmap_start_addr, 0,
				 to_Offset(advance_ptr));
		if (old_value != 0) {
			message << "Zone header init failed" << std::endl;
			throw std::runtime_error(message.str());
		}

		set_bitmap_bit(zoneheader,
			       find_level_from_size(zoneheader->min_object_size, zoneheader->min_object_size),
			       to_Offset(shelf_location_ptr));

		chunk_size = zoneheader->min_object_size;
		advance_ptr = shelf_location_ptr + chunk_size;
		while (chunk_size < initial_pool_size) {
			freelist_level = find_level_from_size(chunk_size, zoneheader->min_object_size);
			ptr = to_Offset(advance_ptr);
			zoneheader->free_list[freelist_level].push(shelf_location_ptr, ptr);
			advance_ptr = (char*)advance_ptr + chunk_size;
			chunk_size = chunk_size << 1;
		}
	} else {
		// Zoneheader and Bitmap fits in different chunks.
                // zoneheader_size was already rounded up to a valid chunk_size
		chunk_size = MAX(zoneheader_size, zoneheader->min_object_size);
                // if the next chunk cannot hold the merge bitmap, it goes to the freelist
		advance_ptr = shelf_location_ptr + chunk_size;
		while (chunk_size < merge_bitmap_size) {
			freelist_level = find_level_from_size(chunk_size, zoneheader->min_object_size);
			ptr = to_Offset(advance_ptr);
			zoneheader->free_list[freelist_level].push(shelf_location_ptr, ptr);
                        // now advance_ptr is the starting address of the next valid chunk
			advance_ptr = (char*)advance_ptr + chunk_size;
			chunk_size = chunk_size << 1;
		}

		old_value = cas64((int64_t *)&zoneheader->merge_bitmap_start_addr, 0,
				 to_Offset(advance_ptr));
		if (old_value != 0) {
			message << "Zone header init failed" << std::endl;
			throw std::runtime_error(message.str());
		}

                // to cover both the zone header and the merge bitmap
		chunk_size = chunk_size << 1;
                // if the next chunk cannot hold the bitmap, it goes to the freelist
		advance_ptr = shelf_location_ptr + chunk_size;
		while (chunk_size < bitmap_size) {
			freelist_level = find_level_from_size(chunk_size, zoneheader->min_object_size);
			ptr = to_Offset(advance_ptr);
			zoneheader->free_list[freelist_level].push(shelf_location_ptr, ptr);
                        // now advance_ptr is the starting address of the next valid chunk
			advance_ptr = (char*)advance_ptr + chunk_size;
			chunk_size = chunk_size << 1;
		}

		old_value = cas64((int64_t *)&zoneheader->bitmap_start_addr, 0,
				 to_Offset(advance_ptr));
		if (old_value != 0) {
			message << "Zone header init failed" << std::endl;
			throw std::runtime_error(message.str());
		}

		// Set bit for the zoneheader chunk
		zoneheader_chunk_size = MAX(zoneheader_size, zoneheader->min_object_size);
		set_bitmap_bit(zoneheader,
			find_level_from_size(zoneheader_chunk_size, zoneheader->min_object_size),
			to_Offset(shelf_location_ptr));

		// Set bit for the merge bitmap chunk
		set_bitmap_bit(zoneheader,
			find_level_from_size(merge_bitmap_size, zoneheader->min_object_size),
			to_Offset(shelf_location_ptr));
                // TODO BUG???: zoneheader->merge_bitmap_start_addr?????

		// Set bit for the bitmap chunk
		set_bitmap_bit(zoneheader,
			find_level_from_size(bitmap_size, zoneheader->min_object_size),
			zoneheader->bitmap_start_addr);

                // Finally, populate the rest of the freelists
		// Cannot populate the freelist before setting the bitmap else alloc will not be able to
		// set the bits on an alloc. 
		chunk_size = chunk_size << 1;
		advance_ptr = shelf_location_ptr + chunk_size;
		while (chunk_size < initial_pool_size) {
			freelist_level = find_level_from_size(chunk_size, zoneheader->min_object_size);
			ptr = to_Offset(advance_ptr);
			zoneheader->free_list[freelist_level].push(shelf_location_ptr, ptr);
			advance_ptr = (char*)advance_ptr + chunk_size;
			chunk_size = chunk_size << 1;
		}

	}
}

/***************************************************************************/
/*                                                                         */
/* Allocating blocks                                                       */
/*                                                                         */
/***************************************************************************/

Offset Zone::alloc(size_t size) 
{
	/*
	1. Identify the freelist level from which we need to seek in free objects.
	2. Loop from the freelist level calculated above to the current zone level
	2.1 Pop a free object from the freelist.
	2.2 If the free object of same size, then set the bitmap and return.
	2.3 Else split the free object until the desired size is reached.

	Spliting an object is simple - break the object into two parts and
	pick up the pointer for the right object and insert it into the stack.
	Again do the same until the desired size is reached.
	*/
	size_t min_obj_size;
	Offset new_chunk_ptr;
	uint64_t current_zone_level, max_zone_level;
	uint64_t orig_freelist_level, level;
	size_t cur_size, chunk_size;
	struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;
	bool grow_in_progress = false;

	// TODO: size > current_zone_size
	min_obj_size = zoneheader->min_object_size;
	//min_obj_size = fam_atomic_u64_read((uint64_t *)&zoneheader->min_object_size);
	chunk_size = next_power_of_two(MAX(size, min_obj_size));
	orig_freelist_level = find_level_from_size(chunk_size, min_obj_size);
//retry:	current_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_zone_level);
retry:	current_zone_level = zoneheader->current_zone_level;
	

	for (level = orig_freelist_level; level <= current_zone_level; level++)
	{
		/*
		TODO: Performance
		We can track an approximate counter for the free list entries. This can
		help in avoiding peeping into empty stack and hence improve performance.
		As the counter is not needed for correctness purpose, we can live with
		stale values.
		*/
		Offset result = zoneheader->free_list[level].pop(shelf_location_ptr);
		if (result) {
			cur_size = find_size_from_level(level, min_obj_size);
			while (level != orig_freelist_level) {
				new_chunk_ptr = result + (cur_size >> 1);
                                // add the second half to the freelist
				zoneheader->free_list[level-1].push(shelf_location_ptr, new_chunk_ptr);
				level--;
				cur_size = cur_size >> 1;
			}
			// Zero out the chunk before returning the pointer to the caller.
			pmem_memset_persist(from_Offset(result), 0, chunk_size);
			set_bitmap_bit(zoneheader, orig_freelist_level, result);
			return append_level_to_Offset(result, orig_freelist_level);
		}
	}
	/* TODO: Should we again check the current_zone_level so that if we a grow that was happening in parallel
	 * has not updated the current_zone_level and this alloc call is looking at older current_zone_level and hence
	 * miss the topmost level to check for a chunk????
	 */
	// Wait for other grow to complete so that we can try again for alloc.
	while (is_grow_in_progress(zoneheader)) {
		// TODO: 1 second wait time is too long. Can we decrease this?
		sleep(1);
		grow_in_progress = true;
	}

	// We actually waited for a grow so let us retry.
	if (grow_in_progress) {
		goto retry;
		//grow_in_progress = false;
	}

	// Before growing try checking if we have indeed reached the last level and cannot grow anymore.
	current_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_zone_level);
	max_zone_level = zoneheader->max_zone_level; 
	//max_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->max_zone_level); 

	if (current_zone_level < max_zone_level) {
		if (grow()) {
			goto retry;
		}
	}

	return -1;
}

/***************************************************************************/
/*                                                                         */
/* Freeing blocks                                                          */
/*                                                                         */
/***************************************************************************/

void Zone::free(Offset block) {

    /*
	1. Find the actual pointer from the Offset received. 
	2. Find the level of the freed object from the Offset received.
	3. For the corresponding level and the pointer, clear the bit in the bitmap.
	4. Push the free object pointer now in the freelist.
    */
    if (block == 0)
        return;

    Offset blk = remove_level_from_Offset(block);
    uint64_t level = get_level_from_Offset(block);
    struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;
    // Do we really need to do pmem_persist???
    //int64_t* b = (int64_t*) from_Offset(blk);
    //pmem_persist(b, find_size_from_level);

    reset_bitmap_bit(zoneheader, level, blk);
    zoneheader->free_list[level].push(shelf_location_ptr, blk);
}

bool Zone::grow()
{
	/*
	1. Grab the lock for the grow so that two grow's cannot happen simultaneously.
	2. Extract the current_zone_level value from the zoneheader. Make sure to do
	   and atomic read.
	3. Increase the value of the current_zone_level and update that in the zoneheader.
	4. Add the free chunk of the grown region to the level-1 freelist.
	5. Clear the lock.
	*/

	struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;
	int64_t old_value;
	uint64_t old_zone_level;
	size_t chunk_size;
	void *advance_ptr;
	std::ostringstream message;
	uint64_t current_zone_level, max_zone_level;

	//uint64_t grow_in_progress = fam_atomic_u64_read((uint64_t *)&zoneheader->grow_in_progress);
	// The grow_in_progress stops many processes to try grow simultaneously.
	// TODO: Use epoch provided value instead of 1 to set the flag.

        // LOCK
	old_value = cas64((int64_t *)&zoneheader->grow_in_progress, 0, 1);
	if (old_value != 0) {
		// Someone else got first in setting the grow flag. So bail out and let it work.
		// As the grow is happening, we should retry allocating.
		return true;
	}

	current_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_zone_level);
	max_zone_level = zoneheader->max_zone_level;
	//max_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->max_zone_level); 

	if (current_zone_level >= max_zone_level) {
                // UNLOCK
		old_value = cas64((int64_t *)&zoneheader->grow_in_progress, 1, 0);
		if (old_value != 1) {
			assert(0);
		}
		return false;
	} else {

		printf("Grow happening from %ld to %ld level\n", current_zone_level, current_zone_level + 1);

		old_zone_level = zoneheader->current_zone_level;
		chunk_size = find_size_from_level(old_zone_level, zoneheader->min_object_size);
		// TODO: Can we just do an atomic increase instead ?
		old_value = cas64((int64_t *)&zoneheader->current_zone_level, old_zone_level, old_zone_level + 1);
		if (old_value != 0 && old_value != (int64_t)old_zone_level) {
			assert(0);
		}

		advance_ptr = shelf_location_ptr + chunk_size;
		zoneheader->free_list[old_zone_level].push(shelf_location_ptr,
								 to_Offset(advance_ptr));

                // UNLOCK
		old_value = cas64((int64_t *)&zoneheader->grow_in_progress, 1, 0);
		if (old_value != 1) {
			assert(0);
		}
		return true;
	}
}

bool Zone::is_grow_in_progress(struct Zone_Header *zoneheader)
{
	uint64_t grow_in_progress = fam_atomic_u64_read((uint64_t *)&zoneheader->grow_in_progress);
	if (grow_in_progress) {
		return true;
	} else {
		return false;
	}
}

inline void set_bit(void *address, uint64_t bit_offset)
{
	uint64_t old_val, new_val;
	int64_t old_value;
	// TODO - Performance:
	// 1. Take the old_val read out of the for loop and use the old_value instead.
	// 2. Can we use a 16 bit or 32 bit read/cas to avoid contention from multiple
	//    processes.
	for(;;) {
		old_val = fam_atomic_u64_read((uint64_t *)address);
		assert((old_val & (1UL << bit_offset)) == 0UL);
		new_val = old_val | (1UL << bit_offset);
		old_value = cas64((int64_t *)address, old_val, new_val);
		if ((uint64_t)old_value == old_val) {
			return;
		}
	}
}

inline void reset_bit(void *address, uint64_t bit_offset)
{
	uint64_t old_val, new_val;
	int64_t old_value;
	// TODO - Performance:
	// 1. Take the old_val read out of the for loop and use the old_value instead.
	// 2. Can we use a 16 bit or 32 bit read/cas to avoid contention from multiple
	//    processes.
	for(;;) {
		old_val = fam_atomic_u64_read((uint64_t *)address);
		assert(((old_val >> bit_offset) & 1UL) == 1UL);
		new_val = old_val & ~(1UL << bit_offset);
		old_value = cas64((int64_t *)address, old_val, new_val);
		if ((uint64_t)old_value == old_val) {
			return;
		}
	}
}


void Zone::modify_bitmap_bit(struct Zone_Header *zoneheader, uint64_t level, Offset ptr, bool set)
{
	uint64_t max_level = zoneheader->max_zone_level;
	//uint64_t max_level = fam_atomic_u64_read((uint64_t *)&zoneheader->max_zone_level);
	uint64_t total_bitmap_bits = (1UL << (max_level + 1));
	uint64_t bitmap_starting_bits_at_level = (1UL << ((max_level - level) + 1));
	uint64_t bitmap_start_at_level, bitmap_offset, bit_offset;
	size_t chunk_size, min_obj_size;
	Offset bitmap_start;
	void *modifying_address;

	// Do we really need to do atomic read for min_object_size and bitmap_start.
	// These are always read-only and wont change ever.
	bitmap_start = zoneheader->bitmap_start_addr;
	min_obj_size = zoneheader->min_object_size;
	//bitmap_start = fam_atomic_u64_read((uint64_t *)&zoneheader->bitmap_start_addr);
	//min_obj_size = fam_atomic_u64_read(&zoneheader->min_object_size);

	bitmap_start_at_level = bitmap_start + ((total_bitmap_bits - bitmap_starting_bits_at_level) / BYTE);
	chunk_size = find_size_from_level(level, min_obj_size);

	bitmap_offset = (ptr / chunk_size) / (BYTE);
	// The last 3 levels are within a single byte. So special care needed to set the proper bits. 
	if (level >= max_level - 1) {
		if (level == max_level - 1) {
			// Second last level starts after 4 bits.
			bit_offset = ((BYTE - 1) - ((ptr / chunk_size) % BYTE)) - 4;
		} else if (level == max_level) {
			// Last level is after 6 bits i.e the 7th bit.
			bit_offset = ((BYTE - 1) - ((ptr / chunk_size) % BYTE)) - 6;
		}
	} else {
		// Calculate from 0-7 instead of 1-8.
		// We only deal with the last 8 bits(SB) in the 64 byte word we read.
		bit_offset = ((BYTE - 1) - ((ptr / chunk_size) % BYTE));
	}
	
	modifying_address = from_Offset(bitmap_start_at_level + bitmap_offset);

	if (set) {
		set_bit(modifying_address, bit_offset);
	} else {
		reset_bit(modifying_address, bit_offset);	
	}
}

void Zone::set_bitmap_bit(struct Zone_Header *zoneheader, uint64_t level, Offset ptr)
{
	modify_bitmap_bit(zoneheader, level, ptr, 1);
}

void Zone::reset_bitmap_bit(struct Zone_Header *zoneheader, uint64_t level, Offset ptr)
{
	modify_bitmap_bit(zoneheader, level, ptr, 0);
}

void Zone::start_merge()
{
	// This is the public interface that can be used by application to start merge. Internally,
	// this API will call merge on each level starting from the lowest level to the highest level.
	
	uint64_t merge_level, current_zone_level;
	struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;

	// TODO: Can we just do fine without atomic read? In worst case, we might skip doing merge
	// for the highest level if the current_zone_level increases. But in such case, it shouldn't
	// matter much as we wont find entries in free-lists.
	current_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_zone_level);

	merge_level = 0;
	while (merge_level < current_zone_level) {
		if (merge(zoneheader, merge_level)) {
			merge_level = merge_level + 1;
		} else {
			std::ostringstream message;
			message << "Aborting merge as two merge processes cannot "
			"run simultaneously" << std::endl;
			throw std::runtime_error(message.str()); 
			break;
		}
	}
}

bool Zone::is_merge_in_progress(struct Zone_Header *zoneheader)
{
	uint64_t merge_in_progress = fam_atomic_u64_read((uint64_t *)&zoneheader->merge_in_progress);
	if (merge_in_progress) {
		return true;
	} else {
		return false;
	}
}

bool Zone::merge(struct Zone_Header *zoneheader, uint64_t level)
{
	/*
	1. Grab the lock for the merge so that two merge cannot happen simultaneously.
	2. Read the current merge_level and update the newer value based on the
	   parameter value.
	3. Swap the freelist head and safe copy head to prevent anyone from using
	   the freelist while merge is happening on that level.
	4. Update the merge status to reflect that the swap is done.
	5. Walk the safe copy as a linked-list and update the merge bitmap to reflect
	   which chunks are present in the freelist.
	6. Update the merge state to reflect that the merge bitmap is updated.
	7. Walk through the merge bitmaps, 64 bit at a time reading from MSB to LSB
	7.1 If the two neighbouring bits are set, then it means that both the blocks
	    are free and hence can be merged together. Upon merging just add it to
	    the post merge level+1 freelist.
	7.2 If the neightbours are not present in the freelist, merge is not possible
	    and hence add it to post merge level freelist.
	8. Update the merge state to reflect that bitmap walk is complete and 
	   merging decisions have to be taken.
	9. Walk through the post_merge freelists and put all the chunks from the
	   freelists to the original freelists.
	10. Clear all the status, current merge level, lock and return.
	*/

	
	Offset level_head;
	Offset safe_copy_head;
	uint64_t old_level;
	int64_t old_value;
	std::ostringstream message;
	uint64_t bitmap_offset, bit_offset, bitmap_data;
	Offset ptr, next_ptr, result;
	size_t chunk_size;
	size_t min_obj_size;
	void *modifying_address;
	Offset merge_bitmap_start;
	uint64_t shift, length = 0, max_bitmap_length;
	int32_t i;
	Offset new_chunk_ptr;
	uint64_t unmerged_chunks = 0, merged_chunks = 0, total_chunks = 0;

	// Merge cannot happen at the max level.
	assert(level < zoneheader->max_zone_level);

	min_obj_size = zoneheader->min_object_size;
	merge_bitmap_start = zoneheader->merge_bitmap_start_addr;

	if (is_merge_in_progress(zoneheader)) {
		return false;
	}
        
	// TODO: Use epoch provided value instead of 1 to set the flag.

        // LOCK
	old_value = cas64((int64_t *)&zoneheader->merge_in_progress, 0, 1);
	if (old_value != 0) {
		// Someone else won the race to start the merge. Return false to let the
		// caller know that someone else started the merge. The caller has to 
		// decide whether to wait or move forward.
		return false;
	}

	assert(zoneheader->merge_status == MERGE_DEFAULT);

	//min_obj_size = fam_atomic_u64_read((uint64_t *)&zoneheader->min_object_size);
	//merge_bitmap_start = fam_atomic_u64_read((uint64_t *)&zoneheader->merge_bitmap_start_addr);
	old_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_merge_level);

	old_value = cas64((int64_t *)&zoneheader->current_merge_level, old_level, level);
	if ((uint64_t)old_value != old_level) {
		message << "Merge failed" << std::endl;
		throw std::runtime_error(message.str());
	}

	printf("Merge happening at level %ld\n", level);

	// TODO: Another method can be to remove just X% of the freelist instead of the entire freelist.
	level_head = fam_atomic_u64_read((uint64_t *)&zoneheader->free_list[level].head);	
	for (;;) {
		// Loop until the safe copy and level freelist are in sync and nobody changes the level
		// freelist before the safe copy is properly copied and the level freelist is cleared. 
retry:		safe_copy_head = fam_atomic_u64_read((uint64_t *)&zoneheader->safe_copy.head);

                // TODO: do we need CAS here? it seems that no one else can be doing merge since we have
                // the merge lock
                // BUG TODO: should we use CAS128? head and the aba_counter
		old_value = cas64((int64_t *)&zoneheader->safe_copy.head, safe_copy_head, level_head);
		if (old_value != (int64_t)safe_copy_head) {
			goto retry;
		}

		old_value = cas64((int64_t *)&zoneheader->free_list[level].head, level_head, 0);
		if (old_value == (int64_t)level_head) {
			break;
		}
		level_head = old_value;
		printf("Trying again\n");
	}

	old_value = cas64((int64_t *)&zoneheader->merge_status, MERGE_DEFAULT, MERGE_SWAP_COMPLETED);
	if (old_value != MERGE_DEFAULT) {
		assert(0);
	}

	// Build the merge bitmap by walking through the entries in the freelist.
	chunk_size = find_size_from_level(level, min_obj_size);

	ptr = zoneheader->safe_copy.head;
	for (;;) {
		if (ptr == 0) {
			break;
		}

		next_ptr = fam_atomic_u64_read((uint64_t *)from_Offset(ptr));

		/*result = zoneheader->safe_copy.pop(shelf_location_ptr);
		if (!result) {
			break;
		}*/
	
		bitmap_offset = (ptr / chunk_size) / (BYTE);
		bit_offset = ((ptr / chunk_size) % (BYTE));
		//bit_offset = (BYTE - 1) - ((result / chunk_size) % (BYTE));
		modifying_address = from_Offset(merge_bitmap_start + bitmap_offset);
		//printf("Result %ld, bitmap_offset %ld, bit_offset %ld\n", result, bitmap_offset, bit_offset);
		set_bit(modifying_address, bit_offset);
		total_chunks = total_chunks + 1;
		ptr = next_ptr;
	}

	old_value = cas64((int64_t *)&zoneheader->merge_status, MERGE_SWAP_COMPLETED, MERGE_BITMAP_COMPLETED);
	if (old_value != MERGE_SWAP_COMPLETED) {
		assert(0);
	}

	max_bitmap_length = ((1UL << (zoneheader->max_zone_level - level)) / BYTE);

	// Special treatment for the last 3 levels as the bitmap size of them is only 1 byte in total.
	if (level + 2 >= zoneheader->max_zone_level) {
		max_bitmap_length = 1;
	}

	while (length < max_bitmap_length) {
		shift = 0xc000000000000000;
		bitmap_offset = merge_bitmap_start + length;
		bitmap_data = fam_atomic_u64_read((uint64_t *)(from_Offset(bitmap_offset))); 
		// No need to check anything in case the entire 64 bytes is 0.
		if (bitmap_data == 0) {
			length = length + 8;
			continue;
		}

		// TODO: Do we need to worry about reading something extra in the last 8 byte read. This problem
		// shouldn't happen as the max_bitmap_length will be a multiple of 8 always until the last few
		// levels where it can be less than 8 bytes. In that case, as the merge_bitmap size is way more
		// than max_bitmap_length and so everything should work fine. 
		for (i = ((8 * BYTE) - 1); i > 0; i = i - 2) {
			if ((bitmap_data & shift) == shift) {
				// Merge the buddies to make a larger chunk.
				new_chunk_ptr = ((BYTE * length) + (i - 1)) * chunk_size;
				//printf("New chunk address %ld, length %ld, i %ld\n", new_chunk_ptr, length, i);
				// As the starting part is always reserved for zone-header, there will
				// never be a case where new_chunk_ptr is 0.
				//TODO: Aseert the bitmap and merge bitmap addresses too.
				assert(new_chunk_ptr != 0); 
				assert(new_chunk_ptr <= zoneheader->max_zone_size);
				zoneheader->post_merge_next_level.push(shelf_location_ptr, new_chunk_ptr);
				merged_chunks = merged_chunks + 2;
			} else {
				// The buddies are not present. Hence keep the chunk at the same level.
				if ((bitmap_data >> i) & 1UL) {
				//if ((bitmap_data >> (((8 * BYTE) - 1) - i)) & 1UL) {
					// The left buddy is present.
					new_chunk_ptr = ((BYTE * length) + i) * chunk_size;
				} else if ((bitmap_data >> (i - 1)) & 1UL) {
					// the right buddy is present.
					new_chunk_ptr = (((BYTE * length) + i) - 1 ) * chunk_size;
				} else {
					shift = shift >> 2;
					continue;
				}
				// As the starting part is always reserved for zone-header, there will
				// never be a case where new_chunk_ptr is 0.
				assert(new_chunk_ptr != 0); 
				assert(new_chunk_ptr <= zoneheader->max_zone_size);
				zoneheader->post_merge_level.push(shelf_location_ptr, new_chunk_ptr);
				unmerged_chunks = unmerged_chunks + 1;
			}
			shift = shift >> 2;
		}
		assert(shift == 0);
		length = length + 8;
	}

	old_value = cas64((int64_t *)&zoneheader->merge_status, MERGE_BITMAP_COMPLETED, MERGE_FREELIST_COMPLETED);
	if (old_value != MERGE_BITMAP_COMPLETED) {
		assert(0);
	}

	printf("Unmerged_chunks = %ld, Merged_chunks = %ld, Total_chunks = %ld\n", unmerged_chunks, merged_chunks, total_chunks);
	assert(unmerged_chunks + merged_chunks  == total_chunks);

	// Again merge back the chunks to the freelist.
	for(;;) {
		result = zoneheader->post_merge_next_level.pop(shelf_location_ptr);
		if (!result) {
			break;
		}
		zoneheader->free_list[level + 1].push(shelf_location_ptr, result);
	}

	for(;;) {
		result = zoneheader->post_merge_level.pop(shelf_location_ptr);
		if (!result) {
			break;
		}
		zoneheader->free_list[level].push(shelf_location_ptr, result);
	}

        old_value = cas64((int64_t *)&zoneheader->current_merge_level, level, 0);
        if ((uint64_t)old_value != level) {
                message << "Merge failed" << std::endl;
                throw std::runtime_error(message.str());
        }

	old_value = cas64((int64_t *)&zoneheader->merge_status, MERGE_FREELIST_COMPLETED, MERGE_DEFAULT);
	if (old_value != MERGE_FREELIST_COMPLETED) {
		assert(0);
	}

	// Zero out the contents of the merge bitmap. 
	pmem_memset_persist(from_Offset(zoneheader->merge_bitmap_start_addr),
			    0, ((1UL << (zoneheader->max_zone_level)) / BYTE));

        
        // UNLOCK
	old_value = cas64((int64_t *)&zoneheader->merge_in_progress, 1, 0);
        if (old_value != 1) {
		// The cas64 shouldn't fail ever as we are the only one doing a merge.
		assert(0);
        }

	return true;
}

bool Zone::IsValidOffset(Offset p)
{
    //return p<zoneheader->max_zone_size; // TODO(y); 
    struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;
    Offset ptr = remove_level_from_Offset(p);
    return ptr>0 && ptr<zoneheader->max_zone_size; 
}

void* Zone::OffsetToPtr(Offset p)
{
    return from_Offset(remove_level_from_Offset(p));
}
    
void Zone::grow_crash_recovery()
{

	// TODO: What if a grow is happening at the same time as the recovery happens? 
	// How do we avoid one process doing a grow and we here are trying to clean
	// things up which we are not supposed to do.

	/*uint64_t grow_in_progress;
	uint64_t current_zone_level, zone_size, level;
	struct Zone_Header *zoneheader = (struct Zone_Header *)shelf_location_ptr;

	grow_in_progress = fam_atomic_u64_read((uint64_t *)&zoneheader->grow_in_progress);

	if (grow_in_progress) {
		current_zone_level = fam_atomic_u64_read((uint64_t *)&zoneheader->current_zone_level);
		zone_size = find_size_from_level(current_zone_level, zoneheader->min_object_size);

		for (level = current_zone_level; level >= 0; level--) {
			ptr = fam_atomic_u64_read((uint64_t *)&zoneheader->free_list[level].head);
			for (;;) {
				if (ptr == 0) {
					break;
				}
				// Let us find any chunk whose address is in the grown region.
				if (ptr < zone_size && ptr >= zone_size / 2) {
				}
			}
		}
		
		old_value = cas64((int64_t *)&zoneheader->grow_in_progress, 1, 0);
		if (old_value != 1) {
			assert(0);
		}	
	}*/

	

	/*
	1. Check for grow flag. If its set, then goto 2. Else just return.
	2. Read the current_zone size.
	3. Based on the current zone size, quickly scan through the higher levels
	   freelists to see if the grown chunk is added or not. 
	4. If we cannot find the chunk in few higher level freelist, then we just
	   consider that the chunk is lost and emit a message to run offline checker.
	5. Clear the flag and continue.
	*/
}

void Zone::merge_crash_recovery()
{
	/*
	1. Check for merge flag. If its set, then goto 2. Else just return.
	2. If merge level is 0, then merge is not yet started. Goto 7.
	3. If swap of the freelist to the safecopy isn't done, then print
	   a message about possible lost blocks and hence run offline checker
	   and return.
	4. If the merge_bitmap is not yet built, then we can restart the
	   merge process again by first building the merge_bitmap and then
	   continue doing the normal merge from there on.
	5. If the freelist is not yet built, then we can restart the merge
	   process again by first building the post-merge freelist and 
	   then continue doing the normal merge from there there on.
	6. If we are done building the freelist and in the middle of pushing
	   the entries to the original freelist, just continue doing it
	   by walking the post merge freelist and complete the merge. Print
	   a message saying that there can be atmost one chunk lost and can be
	   reclaimed by running an offline checker.
	7. Clear merge status, merge level, bitmaps and merge_in_progress flag
	   and return.
	*/
}

void Zone::detect_lost_chunks()
{
	/*
	This is called when there is a possibility of lost block lurking around.
	We do this check when the zone is not available for business else detecting
	lost blocks will be very tricky.
	Assumptions:
	1. The allocation and free are happening when detect_lost_chunks() is
	   executed. We need this assumption so that we can look at the
	   bitmaps and the freelist without worrying about them getting modified.
	2. Grow crash recovery and merge crash recovery is already done before
	   lost chunk detection. We need this assumption so that 

	1. Make sure that grow_crash_recovery() and merge_crash_recovery()
	   is already called.
	2. Copy the lowest level bitmap into the merge_bitmap.
	3. For each level from lowest level + 1 to the max_level, based on the
	   bit set, appropriately set the bits in merge_bitmap.
	4. Start walking through the freelist now from lowest level to the
	   max_level and start setting bits in the merge_bitmap.
	5. All the bits in the merge_bitmap should now be 0xFF. If we find
	   some bits that are not set, then those are the lost blocks and 
	   based on the total number of bits not set, appropriately add the
	   lost blocks in the freelist.
	*/
}

}
