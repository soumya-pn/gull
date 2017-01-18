#ifndef _NVMM_MEMBERSHIP_H_
#define _NVMM_MEMBERSHIP_H_

#include <pthread.h>
#include <assert.h> // for assert()
#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED
#include <stdint.h>
#include <iostream>
#include <string.h> // for memset()
#include <assert.h> // for assert()

#include "nvmm/error_code.h"
#include "nvmm/log.h"
#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/nvmm_libpmem.h"

#include "common/common.h"

namespace nvmm {

// TODO: better bounds checking (size of membership_layout and shared_area is less than the
// shelf size)
struct membership_header
{
    uint64_t magic_num;
    size_t size; // size of the header and the items
    size_t item_count;
};

// cacheline-aligned TypeT
template<class TypeT>
struct CacheLine
{
    TypeT value __attribute__((__aligned__(kCacheLineSize)));

    TypeT *Address()
    {
        return &value;
    }

    void operator=(TypeT const &rhs)
    {
        value = rhs.value;
    }

    operator TypeT()
    {
        return value;
    }

    friend std::ostream& operator<<(std::ostream& os, CacheLine const &cache_line)
    {
        os << cache_line.value;
        return os;
    }
};
    
template<class ItemT, class IndexT>    
class MembershipT
{
public:
    MembershipT() = delete;
    MembershipT(void *addr, size_t avail_size)
        : is_open_{false}, addr_{addr}, size_{avail_size}, item_count_{0}, items_{NULL}
    {
        assert(addr != NULL);
        assert((uint64_t)addr%kCacheLineSize == 0);
    }
    
    ~MembershipT()
    {
        if(IsOpen() == true)
        {
            (void)Close();
        }
    }

    ErrorCode Create(IndexT item_count)
    {
        assert(IsOpen() == false);
        assert(item_count != 0);

        char *cur = (char*)addr_;
        size_t cur_size = size_;
        
        // clear header
        size_t header_size = sizeof(membership_header);
        header_size = round_up(header_size, kCacheLineSize);
        if (cur_size < header_size)
        {
            LOG(error) << "Membership: insufficient space for header";
            return MEMBERSHIP_CREATE_FAILED;
        }
        memset((char*)cur, 0, header_size);        

        // init items
        cur+=header_size;
        cur_size-=header_size;
        size_t items_size = item_count * sizeof(CacheLine<ItemT>);
        items_size = round_up(items_size, kCacheLineSize);
        if (cur_size < items_size)
        {
            LOG(error) << "Membership: insufficient space for free stacks";
            return MEMBERSHIP_CREATE_FAILED;
        }
        memset((char*)cur, 0, items_size);    
        pmem_persist(cur, items_size);

        // set header
        // set item_count
        ((membership_header*)addr_)->item_count = item_count;
        // set size of header and the items
        ((membership_header*)addr_)->size = header_size + items_size;    
        pmem_persist(addr_, header_size);
    
        // finally set magic number
        ((membership_header*)addr_)->magic_num = kMagicNum;
        pmem_persist(addr_, header_size);

        // update actual size
        size_ = ((membership_header*)addr_)->size;
        
        
        return NO_ERROR;
    }
        
    ErrorCode Destroy()
    {
        assert(IsOpen() == false);

        if (Verify() == true)
        {
            size_t size = ((membership_header*)addr_)->size;
            size_ = size;
            memset((char*)addr_, 0, size);    
            pmem_persist(addr_, size);
            return NO_ERROR;
        }
        else
        {
            return MEMBERSHIP_DESTROY_FAILED;
        }
    }

    bool Verify()
    {
        return fam_atomic_u64_read((uint64_t*)addr_) == kMagicNum;
    }

    size_t Size()
    {
        return size_;
    }
    
    bool IsOpen() const
    {
        return is_open_;
    }
    
    ErrorCode Open()
    {
        assert(IsOpen() == false);

        char *cur = (char*)addr_;
    
        // verify header
        if(Verify() == false)
        {
            LOG(error) << "Membership: header->magic_num does not match";
            return MEMBERSHIP_OPEN_FAILED;
        }
        if (((membership_header*)cur)->size > size_)
        {
            LOG(error) << "Membership: insufficient in this shelf";
            return MEMBERSHIP_OPEN_FAILED;
        }

        size_ = ((membership_header*)cur)->size;    
        item_count_ = (IndexT)((membership_header*)cur)->item_count;

        size_t header_size = sizeof(membership_header);
        header_size = round_up(header_size, kCacheLineSize);
        items_ = (CacheLine<ItemT> *)((char*)cur + header_size);
    
        // everything looks good
        is_open_ = true;

        return NO_ERROR;
    }
    
    ErrorCode Close()
    {
        assert(IsOpen() == true);
        is_open_ = false;
        return NO_ERROR;
    }

    /*
      membership APIs
    */
    inline void Print () const
    {
        assert(IsOpen() == true);
        std::cout << "Membership (" << (uint64_t)item_count_ << " items) :" << std::endl;
        for (IndexT i=0; i<item_count_; i++)
        {
            std::cout << items_[i] << std::endl;
        }
        std::cout << std::endl;
    }
    
    inline IndexT Count () const
    {
        return item_count_;
    }

    inline ItemT GetVersionNum(ItemT value)
    {
        return (ItemT)(value & kMaskVersionNum);
    }

    inline bool TestValidBit(ItemT value)
    {
        return (value & kMaskValidBit) != 0;
    }    

    inline ItemT GetItemWithIndex(IndexT index)
    {
        ItemT *address = items_[index].Address();
        return LoadFromFAM(address);
    }

    inline ItemT GetVersionNumWithIndex(IndexT index)
    {
        ItemT *address = items_[index].Address();
        ItemT value = LoadFromFAM(address);
        return GetVersionNum(value);
    }
    
    inline bool TestValidBitWithIndex(IndexT index)
    {
        ItemT *address = items_[index].Address();
        ItemT value = LoadFromFAM(address);
        return TestValidBit(value);
    }

    inline bool TestValidBitWithIndex(IndexT index, ItemT &value)
    {
        ItemT *address = items_[index].Address();
        value = LoadFromFAM(address);
        return TestValidBit(value);
    }

    // ATOMIC
    // Return true if item[index] is used
    // Always return the current value of item[index]
    bool GetUsedSlot (IndexT const index, ItemT &value)
    {
        ItemT *address = items_[index].Address();
        value = LoadFromFAM(address);
        if (TestValidBit(value) == true)
        {
            // valid bit is 1
            return true;
        }
        else
        {
            // valid bit is 0
            return false;
        }
    }

    // ATOMIC
    // Return true only if item[index] was used and we have successfully increased its version
    // number and marked it as free
    // - get the current value of item[index] as expected value
    // - if the valid bit is 1 (used), increase the version number, and flip the valid bit
    // - if the valid bit is 0 (free), return false and the current item value
    // swap against the expected value
    //   - if succeed, return true and the item value before the increase
    //   - if failed, return false and the actual item value
    bool MarkSlotFree (IndexT const index, ItemT &value)
    {
        ItemT *address = items_[index].Address();
        ItemT old_value = LoadFromFAM(address);
        if (TestValidBit(old_value) == true)
        {
            // valid bit is 1
            // increase the version number
            ItemT new_value = IncVersionNum(old_value);
            // clear the valid bit            
            new_value = ClearValidBit(new_value);
            bool ret = CASonFAM(address, &old_value, new_value);
            value = old_value;
            return ret;
        }
        else
        {
            // valid bit is 0
            value = old_value;
            return false;
        }
    }

    
    // ATOMIC
    // Return true only if item[index] is free and we have successfully increased its version number
    // - get the current value of item[index] as expected value
    // - if the valid bit is 1 (used), return false
    // - if the valid bit is 0 (free), increase the version number, and do an atomic compare and
    // swap against the expected value
    //   - if succeed, return true and the item value after the increase
    //   - if fail, return false and the actual item value
    bool GetFreeSlot (IndexT const index, ItemT &value)
    {
        ItemT *address = items_[index].Address();
        ItemT old_value = LoadFromFAM(address);
        if (TestValidBit(old_value) == true)
        {
            // valid bit is 1
            value = old_value;
            return false;
        }
        else
        {
            // valid bit is 0

            // increase the version number
            ItemT new_value = IncVersionNum(old_value);

            bool ret = CASonFAM(address, &old_value, new_value);
            if (ret == true)
            {
                value = new_value;
                assert(TestValidBit(value) == false);                
                return true;
            }
            else
            {
                value = old_value;
                return false;
            }
        }
    }

    // ATOMIC
    // Return true only if item[index] is marked as used by us
    // - use the provided value as expected value
    // - set the valid bit and do an atomic compare and swap against the expected value:
    //   - if succeed, return true and the item value after the valid bit is set
    //   - if fail, return false and the actual item value
    bool MarkSlotUsed (IndexT const index, ItemT &value)
    {
        ItemT *address = items_[index].Address();
        ItemT old_value = value;
        assert(TestValidBit(value) == false);

        // set the valid bit
        ItemT new_value = SetValidBit(value);

        bool ret = CASonFAM(address, &old_value, new_value);
        if (ret == true)
        {
            value = new_value;
            return true;
        }
        else
        {
            value = old_value;
            return false;
        }
    }

    // find the first free slot between start_index and end_index
    bool FindFirstFreeSlot (IndexT & index, IndexT start_index, IndexT end_index)
    {
        start_index = start_index%item_count_;
        end_index = end_index%item_count_;
        if (end_index < start_index)
        {
            for(IndexT i=start_index; i<item_count_; i++)
            {
                if(TestValidBitWithIndex(i) == false)
                {
                    index = i;
                    return true;
                }
            }
            for(IndexT i=0; i<=end_index; i++)
            {
                if(TestValidBitWithIndex(i) == false)
                {
                    index = i;
                    return true;
                }
            }
        }
        else
        {
            for(IndexT i=start_index; i<=end_index; i++)
            {
                if(TestValidBitWithIndex(i) == false)
                {
                    index = i;
                    return true;
                }
            }
        }
        return false;
    }
    
    // find the first used slot between start_index and end_index
    bool FindFirstUsedSlot (IndexT & index, IndexT start_index, IndexT end_index)
    {
        start_index = start_index%item_count_;
        end_index = end_index%item_count_;
        if (end_index < start_index)
        {
            for(IndexT i=start_index; i<item_count_; i++)
            {
                if(TestValidBitWithIndex(i) == true)
                {
                    index = i;
                    return true;
                }
            }
            for(IndexT i=0; i<=end_index; i++)
            {
                if(TestValidBitWithIndex(i) == true)
                {
                    index = i;
                    return true;
                }
            }
        }
        else
        {
            for(IndexT i=start_index; i<=end_index; i++)
            {
                if(TestValidBitWithIndex(i) == true)
                {
                    index = i;
                    return true;
                }
            }
        }
        return false;
    }

    // lock/unlock
    inline void ReadLock()
    {
        pthread_rwlock_rdlock(&rwlock_);
    }

    inline void ReadUnlock()
    {
        pthread_rwlock_unlock(&rwlock_);
    }

    inline void WriteLock()
    {
        pthread_rwlock_wrlock(&rwlock_);
    }

    inline void WriteUnlock()
    {
        pthread_rwlock_unlock(&rwlock_);
    }
    
    inline void Lock()
    {
        pthread_rwlock_wrlock(&rwlock_);
    }

    inline void Unlock()
    {
        pthread_rwlock_unlock(&rwlock_);
    }
    
private:
    static ItemT const kMaskValidBit = (((ItemT)1)<<(sizeof(ItemT)*8-1)); // first bit is the valid bit
    static ItemT const kMaskVersionNum = (((ItemT)1)<<(sizeof(ItemT)*8-1))-1; // the rest bits are the version number

    static uint64_t const kMagicNum = 686362377447; // nvmembership

    // helper functions to access item[]
    inline ItemT LoadFromFAM(ItemT *address)
    {   
        ItemT value = (ItemT)fam_atomic_u64_read((uint64_t*)address);
        return value;
    }

    inline bool CASonFAM(ItemT *address, ItemT *expected, ItemT desired)
    {
        uint64_t result = fam_atomic_u64_compare_and_store((uint64_t*)address,
                                                           (uint64_t)(*expected),
                                                           (uint64_t)desired);
        if(result != (uint64_t)(*expected))
        {
            *expected = (ItemT)result;
            return false;
        }
        else
        {
            return true;
        }
    }

    inline ItemT SetValidBit(ItemT value)
    {
        return (ItemT)(value ^ kMaskValidBit);
    }

    inline ItemT ClearValidBit(ItemT value)
    {
        return (ItemT)(value & (~kMaskValidBit));
    }

    inline ItemT IncVersionNum(ItemT value)
    {
        return (ItemT)((value & kMaskValidBit) ^ (((value & kMaskVersionNum)+1) & kMaskVersionNum));
    }
        
    
    bool is_open_;
    void *addr_;
    size_t size_;

    IndexT item_count_;
    pthread_rwlock_t  rwlock_;
    CacheLine<ItemT> *items_;
};

} // namespace nvmm

#endif
