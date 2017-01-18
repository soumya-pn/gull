#ifndef _NVMM_POOL_H_
#define _NVMM_POOL_H_

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <functional>
#include <pthread.h>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"

#include "common/common.h"

#include "shelf_mgmt/shelf_name.h"
#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/membership.h"

namespace nvmm {

// Pool
// - a pool is a group of related shelves, each of which is identified by a shelf index within a pool
// - the pool id is encoded into the higher 8 bits of the shelf id and the shelf index is
// encoded into the lower 8 bits
// - there is a membership structure to store pool-wide metadata; it is placed inside in a known
// shelf that is private to the pool
// - the pool itself is not thread-safe, but it exposes a set of rwlock APIs so that the client can
// use them to make the pool thread-safe
// TODO:
// - dynamic pool id management, instead of statically assigned pool ids
// - better concurrency control for shelf creation and destroy. Currently we assume the underlying
// file system guarantees the atomicity of file creation, deletion, and rename, and we rely on
// version numbers (which are encoded to file names, hacky!) to guard concurrent shelf creation and
// destroy. Maybe we should use Ownership as "locks"? 
class Pool
{
public:    
    // client-provided function to format the shelf (e.g., set size, preallocate metadata); it is
    // called after the shelf file is created but before the slot is marked as used such that the
    // format is done atomically and no two processes are formatting the same shelf, which could be
    // dangerous)
    // we assume the format function is process-safe and thread-safe; it has to be tolerant to file
    // deletion because an on-going Recover() could be deleting shelf files (the temp version for
    // AddShelf)
    // the format function must return NO_ERROR when formatting is done successfully
    // NOTE: the ShelfFile object is initialized (with a pathname) but not opened
    using FormatFn = std::function<ErrorCode (ShelfFile*, size_t)>;    
    
    static PoolId const kMaxPoolCount = ShelfId::kMaxPoolCount;

    // TODO: support dynamic shelf count
    static ShelfIndex const kMaxShelfCount = ShelfId::kMaxShelfCount;
    // TODO: support variable shelf size
    static size_t const kShelfSize = 128*1024*1024LLU; // 128 MB; TODO: should be 8GB for LFS
    static size_t const kMetadataShelfSize = 128*1024*1024LLU; // 128 MB; TODO: should be 8GB for LFS
    static PoolId const kMetadataPoolId = 0; // pool 0 is reserved to store system-wide metadata;
                                             // e.g., the pool membership is stored in pool 0 

    // convert shelf ids to shelf names
    static ShelfName shelf_name_; 
    
    
    Pool() = delete;
    Pool(PoolId pool_id);
    ~Pool();

    // pool
    ErrorCode Create(size_t shelf_size=kShelfSize);
    ErrorCode Destroy();
    bool Exist();
    bool Verify();
    
    ErrorCode Open(bool recover);
    ErrorCode Close(bool recover);

    bool IsOpen()
    {
        return is_open_;
    }

    // return the max number of shelves in the pool
    ShelfIndex Size()
    {
        return kMaxShelfCount;
    }

    // return a pointer to the shared FAM area
    // this area can be used to store metadata that is shared by all participants using the same pool
    void *SharedArea()
    {
        assert(IsOpen() == true);
        return (char*)addr_+membership_->Size()+kCacheLineSize;
    }

    size_t SharedAreaSize()
    {
        assert(IsOpen() == true);
        return metadata_shelf_.Size()-membership_->Size()-kCacheLineSize;
    }
    
    // recovery procedure
    // it may be racy with concurrent AddShelf and RemoveShelf if the format function for AddShelf
    // does not support multi-process
    // user must decide when it is safe to call Recover()
    ErrorCode Recover();

    // pool membership
    // allocate a new shelf, generate a shelf_idx, and add the shelf to the pool
    ErrorCode NewShelf(ShelfIndex &shelf_idx);
    // allocate a new shelf, try our best to assign it the specified shelf_idx, and add the shelf to
    // the pool
    // if the specified shelf_idx is not available, assign a new one when assign_diff_shelf_idx == true
    ErrorCode AddShelf(ShelfIndex &shelf_idx, bool assign_diff_shelf_idx = true);

    // allocate a new shelf, generate a shelf_idx, and add the shelf to the pool
    ErrorCode NewShelf(ShelfIndex &shelf_idx, FormatFn format_func);
    // allocate a new shelf, try our best to assign it the specified shelf_idx, and add the shelf to
    // the pool
    // if the specified shelf_idx is not available, assign a new one when assign_diff_shelf_idx == true
    ErrorCode AddShelf(ShelfIndex &shelf_idx, FormatFn format_func, bool assign_diff_shelf_idx = true);
    
    // remove the speicified shelf from the pool, and delete the shelf
    // NOTE: caller must make sure the shelf is not being used by others 
    ErrorCode RemoveShelf(ShelfIndex shelf_idx);

    // locate the next available shelf in the pool between start_idx (inclusive) and end_idx (inclusive)
    bool FindNextShelf(ShelfIndex &shelf_idx, ShelfIndex start_idx, ShelfIndex end_idx=kMaxShelfCount-1);
    
    // check if the specified shelf is in the pool    
    bool CheckShelf(ShelfIndex shelf_idx); 

    // shelf usage
    // // return the ShelfFile object of the specified shelf if it exists
    // ErrorCode GetShelfFile(ShelfIndex shelf_idx, ShelfFile *shelf);

    // return the shelf id of the specified shelf if it exists
    ErrorCode GetShelfId(ShelfIndex shelf_idx, ShelfId &shelf_id);

    // decode the shelf_idx from the provided shelf_id if it exists
    ErrorCode GetShelfIdx(ShelfId shelf_id, ShelfIndex &shelf_idx);

    // return the shelf pathname of the specified shelf if it exists
    ErrorCode GetShelfPath(ShelfIndex shelf_idx, std::string &shelf_path);

    
    // helper functions to lock/unlock the pool, just in case the client wants the pool to be thread-safe
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
    
private:
    using Version = uint16_t; // first bit is valid bit; the rest bits are the version number
                              // valid version numbers start from 1; version 0 is reserved, meaning
                              // not versioned
    using Membership = MembershipT<Version, ShelfIndex>;

    // random number generator
    Version RandForAddShelf();
    
    // helper function for Pool::Recover()
    static bool RemoveOldShelfFiles(PoolId pool_id, ShelfIndex shelf_idx, Version version);

    // default format functions
    static ErrorCode DefaultFormatFn(ShelfFile *shelf, size_t shelf_size);
    static ErrorCode TruncateShelfFile(ShelfFile *shelf, size_t shelf_size);

    ErrorCode OpenMapMetadataShelf();
    ErrorCode UnmapCloseMetadataShelf();
    

    PoolId pool_id_;
    bool is_open_;    

    // the private shelf to store pool-wide metadata
    ShelfFile metadata_shelf_;
    void *addr_;
    size_t size_; // size of the metadata_shelf
    
    // pool-wide metadata
    size_t shelf_size_; // stored at addr_ (aligned to CACHE_LINE_SIZE)
    Membership *membership_; // stored at addr_ + CACHE_LINE_SIZE

    pthread_rwlock_t  rwlock_; // guard concurrent access to the pool
};

} // namespace nvmm

#endif
