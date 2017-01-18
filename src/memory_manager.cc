#include <memory>

#include <stddef.h>
#include <stdint.h>
#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED
#include <unistd.h> // for getpagesize()
#include <mutex>
#include <string>
#include <boost/filesystem.hpp>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h" // for PoolId
#include "nvmm/global_ptr.h" // for GlobalPtr
#include "nvmm/heap.h"
#include "nvmm/region.h"

#include "nvmm/log.h"
#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/memory_manager.h"
#include "nvmm/root_shelf.h"
#include "shelf_mgmt/shelf_manager.h"
#include "allocator/pool_region.h"
#ifdef ZONE
#include "allocator/zone_heap.h"
#else
#include "allocator/dist_heap.h"
#endif

namespace nvmm {

/*
 * Internal implemenattion of MemoryManager
 */
class MemoryManager::Impl_
{
public:    
    static std::string const kRootShelfPath; // path of the root shelf

    Impl_()
        : is_ready_(false), root_shelf_(kRootShelfPath), locks_(NULL)
    {        
    }

    ~Impl_()
    {
    }

    ErrorCode Init();
    ErrorCode Final();
        
    ErrorCode MapPointer(GlobalPtr ptr, size_t size,
                         void *addr_hint, int prot, int flags, void **mapped_addr);
    ErrorCode UnmapPointer(GlobalPtr ptr, void *mapped_addr, size_t size);

    void *GlobalToLocal(GlobalPtr ptr);
    GlobalPtr LocalToGlobal(void *addr);
    
    ErrorCode CreateHeap(PoolId id, size_t shelf_size); 
    ErrorCode DestroyHeap(PoolId id);
    ErrorCode FindHeap(PoolId id, Heap **heap);
    Heap *FindHeap(PoolId id);
    
    ErrorCode CreateRegion(PoolId id, size_t size); 
    ErrorCode DestroyRegion(PoolId id);
    ErrorCode FindRegion(PoolId id, Region **region);
    Region *FindRegion(PoolId id);

private:    
    // multi-process/multi-node
    // TODO: NOT resilient to crashes, need the epoch system
    inline void Lock(PoolId pool_id)
    {
        locks_[pool_id].lock();
    }

    inline void Unlock(PoolId pool_id)
    {
        locks_[pool_id].unlock();
    }    

    inline bool TryLock(PoolId pool_id)
    {
        return locks_[pool_id].trylock();
    }    
    
    bool is_ready_;
    RootShelf root_shelf_;
    nvmm_fam_spinlock* locks_; // an array of fam_spinlock    
};
    
std::string const MemoryManager::Impl_::kRootShelfPath = std::string(SHELF_BASE_DIR) + "/" + SHELF_USER + "_NVMM_ROOT";
    
ErrorCode MemoryManager::Impl_::Init()
{
#ifdef LFS
    // TODO
    // when running NVMM on LFS, we must create SHELF_BASE_DIR and the root shelf file during
    // bootstrap (see test/test_common/test.cc)
    // Check if SHELF_BASE_DIR exists
    boost::filesystem::path shelf_base_path = boost::filesystem::path(SHELF_BASE_DIR);    
    if (boost::filesystem::exists(shelf_base_path) == false)
    {
        LOG(fatal) << "NVMM: LFS/tmpfs does not exist?" << SHELF_BASE_DIR;
        exit(1);
    }
    
    if (root_shelf_.Exist() == false)
    {
        LOG(fatal) << "NVMM: Root shelf does not exist?" << kRootShelfPath;
        exit(1);
    }
#else
    // create SHELF_BASE_DIR if it does not exist
    boost::filesystem::path shelf_base_path = boost::filesystem::path(SHELF_BASE_DIR);
    if (boost::filesystem::exists(shelf_base_path) == false)
    {
        bool ret = boost::filesystem::create_directory(shelf_base_path);
        if (ret == false)
        {
            LOG(fatal) << "NVMM: Failed to create SHELF_BASE_DIR " << SHELF_BASE_DIR;
            exit(1);
        }
    }

    // create a root shelf for MemoryManager if it does not exist
    if(root_shelf_.Exist() == false)
    {
        ErrorCode ret = root_shelf_.Create();
        if (ret!=NO_ERROR && ret != SHELF_FILE_FOUND)
        {
            LOG(fatal) << "NVMM: Failed to create the root shelf file " << kRootShelfPath;
            exit(1);
        }
    }
#endif
    int count = 0;
    while(root_shelf_.Open() != NO_ERROR && count < 100)
    {
        LOG(fatal) << "NVMM: Root shelf open failed.. retrying..." << kRootShelfPath;
        usleep(5000);
        count++;
    }
    locks_ = (nvmm_fam_spinlock*)root_shelf_.Addr();
    
    is_ready_ = true;
    return NO_ERROR;
}

ErrorCode MemoryManager::Impl_::Final()
{
    ErrorCode ret = root_shelf_.Close();
    if (ret!=NO_ERROR)
    {
        LOG(fatal) << "NVMM: Root shelf close failed" << kRootShelfPath;
        exit(1);        
    }
        
    is_ready_ = false;
    return NO_ERROR;    
}
    
ErrorCode MemoryManager::Impl_::CreateRegion(PoolId id, size_t size)
{
    assert(is_ready_ == true);
    assert(id > 0);
    ErrorCode ret = NO_ERROR;
    Lock(id);
    PoolRegion pool_region(id);
    ret = pool_region.Create(size);
    Unlock(id);
    if (ret == NO_ERROR)
    {
        return NO_ERROR;
    }    
    if (ret == POOL_FOUND)
    {
        LOG(error) << "MemoryManager: the given id (" << (uint64_t)id << ") is in use";
        return ID_FOUND;
    }
    else
    {
        LOG(fatal) << "MemoryManager: error " << ret;
        return ID_FOUND;
    }
}

// TODO: verify the pool is indeed a region
ErrorCode MemoryManager::Impl_::DestroyRegion(PoolId id)
{
    assert(is_ready_ == true);
    assert(id > 0);
    ErrorCode ret = NO_ERROR;
    Lock(id);
    PoolRegion pool_region(id);
    ret = pool_region.Destroy();
    Unlock(id);
    if (ret == NO_ERROR)
    {
        return NO_ERROR;
    }
    if (ret == POOL_NOT_FOUND)
    {
        LOG(error) << "MemoryManager: region of the given id (" << (uint64_t)id << ") is not found";
        return ID_NOT_FOUND;
    }
    else
    {
        LOG(fatal) << "MemoryManager: error " << ret;
        return ID_NOT_FOUND;
    }
}
    
ErrorCode MemoryManager::Impl_::FindRegion(PoolId id, Region **region)
{
    assert(is_ready_ == true);
    assert(id > 0);
    Lock(id);
    // TODO: use smart poitner or unique pointer?    
    PoolRegion *pool_region = new PoolRegion(id);
    assert(pool_region != NULL); 
    Unlock(id);
    if (pool_region->Exist() == true)
    {
        *region = (Region*)pool_region;
        return NO_ERROR;
    }
    else
    {
        LOG(error) << "MemoryManager: region of the given id (" << (uint64_t)id << ") is not found";
        delete pool_region;
        return ID_NOT_FOUND;
    }
}

Region *MemoryManager::Impl_::FindRegion(PoolId id)
{
    assert(is_ready_ == true);
    assert(id > 0);
    Region *ret = NULL;
    (void)FindRegion(id, &ret);
    return ret;
}   
    
ErrorCode MemoryManager::Impl_::CreateHeap(PoolId id, size_t size)
{
    assert(is_ready_ == true);
    assert(id > 0);
    ErrorCode ret = NO_ERROR;
    Lock(id);
#ifdef ZONE
    ZoneHeap heap(id);
#else
    DistHeap heap(id);    
#endif
    ret = heap.Create(size);
    Unlock(id);
    if (ret == NO_ERROR)
    {
        return NO_ERROR;
    }    
    if (ret == POOL_FOUND)
    {
        LOG(error) << "MemoryManager: the given id (" << (uint64_t)id << ") is in use";
        return ID_FOUND;
    }
    else
    {
        LOG(fatal) << "MemoryManager: error " << ret;
        return ID_FOUND;
    }
}

ErrorCode MemoryManager::Impl_::DestroyHeap(PoolId id)
{
    assert(is_ready_ == true);
    assert(id > 0);
    ErrorCode ret = NO_ERROR;
    Lock(id);
#ifdef ZONE
    ZoneHeap heap(id);
#else
    //PoolHeap heap(id);
    DistHeap heap(id);
#endif
    ret = heap.Destroy();
    Unlock(id);
    if (ret == NO_ERROR)
    {
        return NO_ERROR;
    }
    if (ret == POOL_NOT_FOUND)
    {
        LOG(error) << "MemoryManager: heap of the given id (" << (uint64_t)id << ") is not found";
        return ID_NOT_FOUND;
    }
    else
    {
        LOG(fatal) << "MemoryManager: error " << ret;
        return ID_NOT_FOUND;
    }
}
    
ErrorCode MemoryManager::Impl_::FindHeap(PoolId id, Heap **heap)
{
    assert(is_ready_ == true);
    assert(id > 0);
    Lock(id);
    // TODO: use smart poitner or unique pointer?
#ifdef ZONE
    ZoneHeap *heap_ = new ZoneHeap(id);
#else
    DistHeap *heap_ = new DistHeap(id);
#endif    
    assert(heap_ != NULL);
    Unlock(id);
    if(heap_->Exist() == true)
    {
        *heap = (Heap*)heap_;
        return NO_ERROR;
    }
    else
    {
        LOG(error) << "MemoryManager: heap of the given id (" << (uint64_t)id << ") is not found";
        return ID_NOT_FOUND;
    }
}

Heap *MemoryManager::Impl_::FindHeap(PoolId id)
{
    assert(is_ready_ == true);
    assert(id > 0);
    Heap *ret = NULL;
    (void)FindHeap(id, &ret);
    return ret;
}   

ErrorCode MemoryManager::Impl_::MapPointer(GlobalPtr ptr, size_t size,
                                    void *addr_hint, int prot, int flags, void **mapped_addr)
{
    assert(is_ready_ == true);

    if (ptr.IsValid() == false)
    {
        LOG(error) << "MemoryManager: Invalid Global Pointer: " << ptr;
        return INVALID_PTR;
    }
    
    ErrorCode ret = NO_ERROR;
    ShelfId shelf_id = ptr.GetShelfId();
    PoolId pool_id = shelf_id.GetPoolId();
    if (pool_id == 0)
    {
        LOG(error) << "MemoryManager: Invalid Global Pointer: " << ptr;
        return INVALID_PTR;
    }
    ShelfIndex shelf_idx = shelf_id.GetShelfIndex();
    Offset offset = ptr.GetOffset();

    // handle alignment
    int page_size = getpagesize();
    off_t aligned_start = offset - offset % page_size;
    assert(aligned_start % page_size == 0);
    off_t aligned_end = (offset + size) + (page_size - (offset + size) % page_size);
    assert(aligned_end % page_size == 0);
    size_t aligned_size = aligned_end - aligned_start;
    assert(aligned_size % page_size == 0);

    void *aligned_addr = NULL;
    
    // TODO: this is way too costly
    // open the pool
    Pool pool(pool_id);
    ret = pool.Open(false);
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    std::string shelf_path;
    ret = pool.GetShelfPath(shelf_idx, shelf_path);
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    ShelfFile shelf(shelf_path);
    
    // open the shelf file
    ret = shelf.Open(O_RDWR);
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    // only mmap offset + size
    ret = shelf.Map(addr_hint, aligned_size, PROT_READ|PROT_WRITE, MAP_SHARED, aligned_start,
                    &aligned_addr, false);
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    // close the shelf file
    ret = shelf.Close();
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    // close the pool
    ret = pool.Close(false);
    if (ret != NO_ERROR)
    {
        return MAP_POINTER_FAILED;
    }

    *mapped_addr = (void*)((char*)aligned_addr + offset % page_size);

    LOG(trace) << "MapPointer: path " << shelf.GetPath()
               << " offset " << aligned_start << " size " << aligned_size
               << " aligned ptr " << (void*)aligned_addr
               << " returned ptr " << (void*)(*mapped_addr);

    return ret;

}
    

ErrorCode MemoryManager::Impl_::UnmapPointer(GlobalPtr ptr, void *mapped_addr, size_t size)
{ 
    assert(is_ready_ == true);
    Offset offset = ptr.GetOffset();
    int page_size = getpagesize();
    off_t aligned_start = offset - offset % page_size;
    assert(aligned_start % page_size == 0);
    off_t aligned_end = (offset + size) + (page_size - (offset + size) % page_size);
    assert(aligned_end % page_size == 0);
    size_t aligned_size = aligned_end - aligned_start;
    assert(aligned_size % page_size == 0);
    void *aligned_addr = (void*)((char*)mapped_addr - offset % page_size);
    LOG(trace) << "UnmapPointer: path " << " offset " << aligned_start << " size " << aligned_size
               << " aligned ptr " << (void*)aligned_addr
               << " input ptr " << (void*)mapped_addr;
    return ShelfFile::Unmap(aligned_addr, aligned_size, false);
}

void *MemoryManager::Impl_::GlobalToLocal(GlobalPtr ptr)
{
    assert(is_ready_ == true);

    if (ptr.IsValid() == false)
    {
        LOG(error) << "MemoryManager: Invalid Global Pointer: " << ptr;
        return NULL;
    }
    
    ErrorCode ret = NO_ERROR;
    ShelfId shelf_id = ptr.GetShelfId();
    Offset offset = ptr.GetOffset();
    void *addr = ShelfManager::FindBase(shelf_id);
    if (addr == NULL)
    {
        // slow path
        // first time accessing this shelf        
        PoolId pool_id = shelf_id.GetPoolId();
        if (pool_id == 0)
        {
            LOG(error) << "MemoryManager: Invalid Global Pointer: " << ptr;
            return NULL;
        }
        // TODO: this is way too expensive
        // open the pool
        Pool pool(pool_id);
        ret = pool.Open(false);
        if (ret != NO_ERROR)
        {
            return NULL;
        }

        ShelfIndex shelf_idx = shelf_id.GetShelfIndex();
        std::string shelf_path;    
        ret = pool.GetShelfPath(shelf_idx, shelf_path);
        if (ret != NO_ERROR)
        {
            return NULL;
        }

        addr = ShelfManager::FindBase(shelf_path, shelf_id);

        // close the pool
        (void) pool.Close(false);        
    }

    if (addr != NULL)
    {
        addr = (void*)((char*)addr+offset);
        LOG(trace) << "GetLocalPtr: global ptr" << ptr
                   << " offset " << offset
                   << " returned ptr " << (uintptr_t)addr;
    }
    
    return addr;
}

// TODO(zone): how to make this work for zone ptr?
GlobalPtr MemoryManager::Impl_::LocalToGlobal(void *addr)
{
#ifdef ZONE
    LOG(fatal) << "WARNING: LocalToGlobal is currenly not supported for Zone";
    return GlobalPtr();
#else    
    void *base = NULL;
    ShelfId shelf_id = ShelfManager::FindShelf(addr, base);
    if (shelf_id.IsValid() == false)
    {
        LOG(error) << "GetGlobalPtr failed";
        return GlobalPtr(); // return an invalid global pointer
    }
    else
    {
        Offset offset = (uintptr_t)addr - (uintptr_t)base;
        GlobalPtr global_ptr = GlobalPtr(shelf_id, offset);
        LOG(trace) << "GetGlobalPtr: local ptr " << (uintptr_t)addr
                   << " offset " << offset
                   << " returned ptr " << global_ptr;
        return global_ptr;
    }
#endif    
}



/*
 * Public APIs of MemoryManager
 */       
    
// thread-safe Singleton pattern with C++11
// see http://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11/
MemoryManager *MemoryManager::GetInstance()
{
    static MemoryManager instance;
    return &instance;
}

MemoryManager::MemoryManager()
    : pimpl_{new Impl_}
{
    ErrorCode ret = pimpl_->Init();
    assert(ret == NO_ERROR);
}

MemoryManager::~MemoryManager()
{
    ErrorCode ret = pimpl_->Final();
    assert(ret == NO_ERROR);
}   

ErrorCode MemoryManager::CreateRegion(PoolId id, size_t size)
{
    return pimpl_->CreateRegion(id, size);
}

ErrorCode MemoryManager::DestroyRegion(PoolId id)
{
    return pimpl_->DestroyRegion(id);
}
    
ErrorCode MemoryManager::FindRegion(PoolId id, Region **region)
{
    return pimpl_->FindRegion(id, region);
}

Region *MemoryManager::FindRegion(PoolId id)
{
    return pimpl_->FindRegion(id);
}   
    
ErrorCode MemoryManager::CreateHeap(PoolId id, size_t size)
{
    return pimpl_->CreateHeap(id, size);
}

ErrorCode MemoryManager::DestroyHeap(PoolId id)
{
    return pimpl_->DestroyHeap(id);
}
    
ErrorCode MemoryManager::FindHeap(PoolId id, Heap **heap)
{
    return pimpl_->FindHeap(id, heap);
}

Heap *MemoryManager::FindHeap(PoolId id)
{
    return pimpl_->FindHeap(id);
}   

ErrorCode MemoryManager::MapPointer(GlobalPtr ptr, size_t size,
                                    void *addr_hint, int prot, int flags, void **mapped_addr)
{
    return pimpl_->MapPointer(ptr, size, addr_hint, prot, flags, mapped_addr);
}
    

ErrorCode MemoryManager::UnmapPointer(GlobalPtr ptr, void *mapped_addr, size_t size)
{ 
    return pimpl_->UnmapPointer(ptr, mapped_addr, size);
}

void *MemoryManager::GlobalToLocal(GlobalPtr ptr)
{
    return pimpl_->GlobalToLocal(ptr);
}

GlobalPtr MemoryManager::LocalToGlobal(void *addr)
{
    return pimpl_->LocalToGlobal(addr);
}
    

} // namespace nvmm
