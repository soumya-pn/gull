#include <stddef.h>
#include <stdint.h>
#include <thread>
#include <mutex>
#include <pthread.h> // the reader-writer lock
#include <map>

#include <assert.h>
#include <string>
#include <unistd.h>

#include "nvmm/error_code.h"
#include "nvmm/global_ptr.h"
#include "nvmm/shelf_id.h"
#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/nvmm_libpmem.h"
#include "nvmm/heap.h"

#include "shelf_mgmt/pool.h"

#include "shelf_usage/ownership.h"
#include "shelf_usage/freelists.h"
#include "shelf_usage/shelf_heap.h"

#include "allocator/dist_heap.h"

namespace nvmm {
   
DistHeap::DistHeap(PoolId pool_id)
    : pool_id_{pool_id}, pool_{pool_id}, is_open_{false},
      ownership_{NULL}, freelists_{NULL},
      cleaner_running_{false}, cleaner_stop_{false}
{
    int rc = pthread_rwlock_init(&rwlock_, NULL);
    assert(rc == 0);
}
    
DistHeap::~DistHeap()
{
    if(IsOpen() == true)
    {
        (void)Close();
    }
}

ErrorCode DistHeap::Create(size_t shelf_size)
{
    TRACE();
    assert(IsOpen() == false);
    if (pool_.Exist() == true)
    {
        return POOL_FOUND;
    }
    else
    {
        ErrorCode ret = NO_ERROR;

        // create an empty pool
        ret = pool_.Create(shelf_size);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Pool create failed";
            if (ret == POOL_FOUND)
                return POOL_FOUND;
            else
                return HEAP_CREATE_FAILED;
        }

        // set up heap metadata
        ret = pool_.Open(false);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Pool open failed";
            return HEAP_CREATE_FAILED;
        }

        char *shared_addr = (char*)pool_.SharedArea();
        size_t shared_size = pool_.SharedAreaSize();        
        size_t used_size = 0;

        // create ownership        
        Ownership ownership((void*)shared_addr, shared_size);
        ret = ownership.Create(kMaxShelfCount);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Ownership create failed";
            return HEAP_CREATE_FAILED;
        }
        used_size = ownership.Size();
        
        // create freelists
        shared_addr+=used_size;
        shared_size-=used_size;
        FreeLists freelists((void*)shared_addr, shared_size);        
        ret = freelists.Create(kMaxShelfCount);
        if (ret != NO_ERROR)
        {
            LOG(error) << "FreeLists create failed";
            return HEAP_CREATE_FAILED;
        }
        used_size = freelists.Size();
        assert(shared_size == used_size);
        
        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }
        
        return ret;
    }
}

ErrorCode DistHeap::Destroy()
{
    TRACE();
    assert(IsOpen() == false);    
    if (pool_.Exist() == false)
    {
        return POOL_NOT_FOUND;
    }
    else
    {
        ErrorCode ret = NO_ERROR;

        // remove all shelves
        ret = pool_.Open(false);
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        ret = pool_.Recover();
        if(ret != NO_ERROR)
        {
            LOG(fatal) << "Destroy: Found inconsistency in Heap " << (uint64_t)pool_id_;
        }        
        for (ShelfIndex shelf_idx = 0; shelf_idx < pool_.Size(); shelf_idx++)
        {
            if (pool_.CheckShelf(shelf_idx) == true)
            {
                std::string path;
                ret = pool_.GetShelfPath(shelf_idx, path);
                assert(ret == NO_ERROR);
                ShelfHeap shelf_heap(path, ShelfId(pool_id_, shelf_idx));
                shelf_heap.Destroy();
                ret = pool_.RemoveShelf(shelf_idx);
                if (ret != NO_ERROR)
                {
                    (void)pool_.Close(false);
                    return HEAP_DESTROY_FAILED;
                }
            }
        }

        // char *shared_addr = (char*)pool_.SharedArea();
        // size_t shared_size = pool_.SharedAreaSize();
        // size_t used_size = 0;        

        // // destroy ownership        
        // Ownership ownership((void*)shared_addr, shared_size);
        // ret = ownership.Destroy();
        // if (ret != NO_ERROR)
        // {
        //     return HEAP_DESTROY_FAILED;
        // }
        // used_size = ownership.Size();
        
        // // destroy freelists
        // shared_addr+=used_size;
        // shared_size-=used_size;
        // FreeLists freelists((void*)shared_addr, shared_size);
        // ret = freelists.Destroy();
        // if (ret != NO_ERROR)
        // {
        //     return HEAP_DESTROY_FAILED;
        // }
        // used_size = freelists.Size();
        // assert(shared_size == used_size);
        
        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        
        ret = pool_.Destroy();
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        return ret;
    }
}

bool DistHeap::Exist()
{
    return pool_.Exist();
}

ErrorCode DistHeap::Open()
{
    TRACE();
    LOG(trace) << "Open Heap " << (uint64_t) pool_id_;
    assert(IsOpen() == false);
    ErrorCode ret = NO_ERROR;

    // open the pool
    ret = pool_.Open(false);
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }

    // // perform recovery
    // ret = pool_.Recover(
    //                     [](ShelfFile *shelf)
    //                     {
    //                         return ShelfHeap::Verify(shelf);
    //                     }
    //                     );
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Open: Found inconsistency in Heap " << (uint64_t)pool_id_;
    // }
    
    char *shared_addr = (char*)pool_.SharedArea();
    size_t shared_size = pool_.SharedAreaSize();
    size_t used_size = 0;
    
    // init ownership
    ownership_ = new Ownership((void*)shared_addr, shared_size);
    assert(ownership_ != NULL);
    ret = ownership_->Open();
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }    
    used_size = ownership_->Size();
    
    // init freelists
    shared_addr+=used_size;
    shared_size-=used_size;
    freelists_ = new FreeLists((void*)shared_addr, shared_size);
    assert(freelists_ != NULL);
    ret = freelists_->Open();
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }    
    used_size = freelists_->Size();
    assert(used_size == shared_size);
    
    // TODO: load freespace stats

    // own a heap
    WriteLock();
    ShelfIndex shelf_idx;
    if (AcquireShelfHeap(shelf_idx, false) == true)
    {
        LOG(trace) << "Acquiring a new heap (Open) " << (uint64_t)shelf_idx;        
        ret = OpenShelfHeap(shelf_idx);
        if (ret != NO_ERROR)
        {
            LOG(error) << "OpenShelfHeap failed";
            WriteUnlock();
            return HEAP_OPEN_FAILED;
        }
    }
    WriteUnlock();

    is_open_ = true;

    // start the cleaner thread
    int rc = StartWorker();
    if (rc != 0)
    {
        return HEAP_OPEN_FAILED;
    }
    
    assert(ret == NO_ERROR);        
    return ret;
}

ErrorCode DistHeap::Close()
{
    TRACE();
    LOG(trace) << "Close " << (uint64_t)pool_id_;
    assert(IsOpen() == true);
    ErrorCode ret = NO_ERROR;

    // stop the cleaner thread
    int rc = StopWorker();
    if (rc != 0)
    {
        return HEAP_CLOSE_FAILED;
    }
    
    // close opened heaps
    WriteLock();
    for (auto it = map_.begin(); it != map_.end();)
    {
        ShelfIndex shelf_idx = it->first;
        it ++;
        ret = CloseShelfHeap(shelf_idx);
        if (ret != NO_ERROR)
        {
            LOG(error) << "CloseShelfHeap failed";
            WriteUnlock();
            return HEAP_CLOSE_FAILED;
        }
        if (ReleaseShelfHeap(shelf_idx) == false)
        {
            LOG(fatal) << "BUG: Close";
            WriteUnlock();
            return HEAP_CLOSE_FAILED;
        }        
    }
    WriteUnlock();

    // close ownership
    ret = ownership_->Close();
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    delete ownership_;
    ownership_ = NULL;
    
    // close freelists
    ret = freelists_->Close();
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    
    // perform recovery
    // ret = pool_.Recover(
    //                     [](ShelfFile *shelf)
    //                     {
    //                         return ShelfHeap::Verify(shelf);
    //                     }
    //                     );
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Close: Found inconsistency in Heap " << pool_id_;
    // }
        

    // close the pool
    ret = pool_.Close(false);
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }

    is_open_ = false;
    assert(ret == NO_ERROR);    
    return ret;
}

GlobalPtr DistHeap::Alloc (size_t size)
{
    TRACE();
    assert(IsOpen() == true);
    ErrorCode ret = NO_ERROR;
    GlobalPtr ptr;

    ReadLock();
    for (auto& it : map_)
    {
        ShelfIndex shelf_idx = it.first;
        ShelfHeap *shelf_heap = it.second;
        Offset offset = shelf_heap->Alloc(size);
        if (shelf_heap->IsValidOffset(offset) == true)
        {
            // allocation succeeded
            ShelfId shelf_id(pool_id_, shelf_idx);
            ptr = GlobalPtr(shelf_id, offset);
            LOG(trace) << "Allocation succeeded at heap " << (uint64_t)shelf_idx << " " << ptr;
            ReadUnlock();
            return ptr;
        }
        else
        {
            LOG(trace) << "Allocation failed at heap " << (uint64_t)shelf_idx;
        }
    }
    ReadUnlock();

    // we have tried all heaps we own
    // now we should try to acquire a new heap
    WriteLock();
    if (map_.size() == kMaxOwnedHeap)
    {
        // we have reached the limit; we must release a heap in order to own one
        // we release the first heap (which is likedly to be full)
        auto it = map_.begin();
        ShelfIndex shelf_idx = it->first;        
        ret = CloseShelfHeap(shelf_idx);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Alloc: CloseShelfHeap failed";
            WriteUnlock();
            return ptr;
        }
        if (ReleaseShelfHeap(shelf_idx) == false)
        {
            LOG(fatal) << "Alloc: BUG ReleaseShelfHeap failed";
            WriteUnlock();
            return ptr;
        }
    }

    // try to find an existing heap
    ShelfIndex shelf_idx;
    if (AcquireShelfHeap(shelf_idx, false) == true)
    {
        LOG(trace) << "Acquiring a new heap " << (uint64_t)shelf_idx;
        ret = OpenShelfHeap(shelf_idx);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Alloc: OpenShelfHeap failed";
            WriteUnlock();
            return ptr;
        }
        ShelfHeap *shelf_heap = LookupShelfHeap(shelf_idx);
        Offset offset = shelf_heap->Alloc(size);
        if (shelf_heap->IsValidOffset(offset) == true)
        {
            // allocation succeeded
            ShelfId shelf_id(pool_id_, shelf_idx);
            ptr = GlobalPtr(shelf_id, offset);
            LOG(trace) << "Allocation succeeded at heap " << (uint64_t)shelf_idx << " " << ptr;
            WriteUnlock();
            return ptr;
        }
        else
        {
            // allocation failed
            LOG(trace) << "Allocation failed at heap " << (uint64_t)shelf_idx << " " << ptr;
            ret = CloseShelfHeap(shelf_idx);
            if (ret != NO_ERROR)
            {
                LOG(error) << "Alloc: CloseShelfHeap failed";
                WriteUnlock();
                return ptr;
            }
            if (ReleaseShelfHeap(shelf_idx) == false)
            {
                LOG(fatal) << "Alloc: BUG ReleaseShelfHeap failed";
                WriteUnlock();
                return ptr;
            }            
        }
    }
    else
    {
        LOG(trace) << "Failed to acquire a new heap";
    }

    // last resort: create a new heap
    if (AcquireShelfHeap(shelf_idx, true) == true)
    {
        LOG(trace) << "Acquiring a new heap (retry) " << (uint64_t)shelf_idx;
        ret = OpenShelfHeap(shelf_idx);
        if (ret != NO_ERROR)
        {
            LOG(error) << "Alloc: OpenShelfHeap failed";
            WriteUnlock();
            return ptr;
        }
        ShelfHeap *shelf_heap = LookupShelfHeap(shelf_idx);
        Offset offset = shelf_heap->Alloc(size);
        if (shelf_heap->IsValidOffset(offset) == true)
        {
            // allocation succeeded
            ShelfId shelf_id(pool_id_, shelf_idx);
            ptr = GlobalPtr(shelf_id, offset);
            LOG(trace) << "Allocation succeeded at heap " << (uint64_t)shelf_idx << " " << ptr;
            WriteUnlock();
            return ptr;
        }
        else
        {
            // allocation succeeded
            LOG(trace) << "Allocation failed at heap " << (uint64_t)shelf_idx << " " << ptr;
        }
    }
    else
    {
        LOG(trace) << "Failed to acquire a new heap";
    }

    
    WriteUnlock();

    return ptr;
}

// TODO: Free may fail if the freelists are full....
void DistHeap::Free (GlobalPtr global_ptr)
{
    TRACE();
    assert(IsOpen() == true);
    ErrorCode ret;

    ShelfId shelf_id = global_ptr.GetShelfId();
    Offset offset = global_ptr.GetOffset();
    PoolId pool_id = shelf_id.GetPoolId();
    ShelfIndex shelf_idx = shelf_id.GetShelfIndex();

    assert(pool_id == pool_id_);

    ReadLock();    
    ShelfHeap *shelf_heap = LookupShelfHeap(shelf_idx);
    if (shelf_heap != NULL)
    {
        // local free
        shelf_heap->Free(offset);
        ReadUnlock();    
    }
    else
    {
        // remote free
        ReadUnlock();

        // TODO: how to make sure the shelf_idx is valid?
        assert(pool_.CheckShelf(shelf_idx)==true);
        ret = freelists_->PutPointer(shelf_idx, global_ptr);
        if (ret != NO_ERROR)
        {
            LOG(fatal) << "Freelist is running out of space...";
            exit(1);
        }
    }

    return;
}
    
void *DistHeap::GlobalToLocal(GlobalPtr global_ptr)
{
    TRACE();  
    assert(IsOpen() == true);
    void *local_ptr = NULL;

    ShelfId shelf_id = global_ptr.GetShelfId();
    Offset offset = global_ptr.GetOffset();
    PoolId pool_id = shelf_id.GetPoolId();
    ShelfIndex shelf_idx = shelf_id.GetShelfIndex();

    assert(pool_id == pool_id_);
    
    ReadLock();    
    ShelfHeap *shelf_heap = LookupShelfHeap(shelf_idx);
    if (shelf_heap != NULL)
    {
        assert(shelf_heap->IsValidOffset(offset));
        local_ptr = shelf_heap->OffsetToPtr(offset);
    }
    else
    {
        LOG(fatal) << "GlobalToLocal: LookupShelfHeap failed";
    }
    ReadUnlock();

    return local_ptr;
}

// TODO: not implemented
// GlobalPtr DistHeap::LocalToGlobal(void *addr)
// {
//     TRACE();  
//     return global_ptr;
// }

int DistHeap::StartWorker()
{
    // start the cleaner thread
    std::lock_guard<std::mutex> mutex(cleaner_mutex_);
    if (cleaner_running_ == true)
    {
        LOG(trace) << " cleaner thread is already running...";
        return 0;
    }
    cleaner_stop_ = false;
    cleaner_running_ = true;
    cleaner_thread_ = std::thread(&DistHeap::BackgroundWorker, this);
    return 0;
}    

int DistHeap::StopWorker()
{
    // signal the cleaner to stop
    {
        std::lock_guard<std::mutex> mutex(cleaner_mutex_);
        if (cleaner_running_ == false)
        {
            LOG(trace) << " cleaner thread is not running...";
            return 0;
        }
        cleaner_stop_ = true;
    }
        
    // join the cleaner thread
    if(cleaner_thread_.joinable())
    {
        cleaner_thread_.join();
    }

    {
        std::lock_guard<std::mutex> mutex(cleaner_mutex_);
        cleaner_stop_ = false;
        cleaner_running_ = false;
    }
    return 0;
}    

void DistHeap::BackgroundWorker()
{
    TRACE();  
    assert(IsOpen() == true);

    while(1)
    {
        LOG(trace) << "cleaner: sleep";
        usleep(kWorkerSleepMicroSeconds);
        LOG(trace) << "cleaner: wakeup";

        // check if we are shutting down...
        {
            std::lock_guard<std::mutex> mutex(cleaner_mutex_);
            if (cleaner_stop_ == true)
            {
                LOG(trace) << "cleaner: exiting...";
                goto out;
            }
        }

        // check ownership and do recovery if inconsistency is found
        LOG(trace) << "cleaner: consistency checking";
        for (size_t i=0; i<ownership_->Count(); i++)
        {
            ownership_->CheckAndRevokeItem(i,
                                           [this](ShelfIndex shelf_idx)
                                           {
                                               return RecoverShelfHeap(shelf_idx);
                                           }
                                           );
        }
        
        // clean up freelists
        // TODO: holding readlock may prevent someone from owning a new heap
        ReadLock();
        for (auto& it : map_)
        {
            ShelfIndex shelf_idx = it.first;            
            GlobalPtr ptr;
            // free one pointer from one heap per iteration
            ErrorCode ret = freelists_->GetPointer(shelf_idx, ptr);
            if (ret == NO_ERROR)
            {
                LOG(trace) << "cleaner: free ptr " << ptr;            
                Free(ptr);
                continue;
            }
            else
            {
                LOG(trace) << "cleaner: freelist is empty";
                continue;
            }
        }
        ReadUnlock();
    }
 out:    
    pthread_exit(NULL);    
}
    
bool DistHeap::AcquireShelfHeap(ShelfIndex &shelf_idx, bool newonly)
{
    if (newonly == false)
    {
        // try to find a heap that exists but is not owned by anyone
        for (ShelfIndex i = 0; i < (ShelfIndex)ownership_->Count(); i++)
        {
            if (ownership_->CheckItem(i) == false)
            {
                if (pool_.CheckShelf(i) == true)
                {
                    if (ownership_->AcquireItem(i) == true)
                    {
                        shelf_idx = i;
                        return true;
                    }
                }
            }
        }
    }
    
    // it seems that all existing heaps are owned
    // try to create a new heap
    for (ShelfIndex i = 0; i < (ShelfIndex)ownership_->Count(); i++)
    {
        if (ownership_->CheckItem(i) == false)
        {
            if (pool_.CheckShelf(i) == true)
            {
                if (ownership_->AcquireItem(i) == true)
                {
                    shelf_idx = i;
                    return true;
                }
            }
            else
            {
                if (ownership_->AcquireItem(i) == true)
                {
                    ErrorCode ret = pool_.AddShelf(i,
                                                   [](ShelfFile *shelf, size_t size)
                                         {
                                             ShelfHeap shelf_heap(shelf->GetPath());
                                             return shelf_heap.Create(size);
                                         },
                                         false // must use the specified shelf_idx
                                         );
                    if (ret != NO_ERROR)
                    {
                        if (ownership_->ReleaseItem(i) != true)
                        {
                            LOG(fatal) << "BUG: AcquireShelfHeap";
                            return false;
                        }
                    }
                    else
                    {
                        shelf_idx = i;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool DistHeap::ReleaseShelfHeap(ShelfIndex shelf_idx)
{
    // TODO: verify that the caller does own this heap    
    assert(ownership_->CheckItem(shelf_idx) == true);
    assert(pool_.CheckShelf(shelf_idx) == true);
    if (ownership_->ReleaseItem(shelf_idx) != true)
    {
        LOG(fatal) << "BUG: ReleaseShelfHeap";
        return false;
    }
    return true;
}

bool DistHeap::RegisterShelfHeap(ShelfIndex shelf_idx, ShelfHeap *shelf_heap)
{
    auto entry = std::make_pair(shelf_idx, shelf_heap);
    auto result = map_.insert(entry);
    if (result.second == true)
    {
        LOG(trace) << "RegisterShelfHeap: mapping registered";
        return true;
    }
    else
    {
        LOG(trace) << "RegisterShelfHeap: existing mapping";
        return false;
    }
}

bool DistHeap::UnregisterShelfHeap(ShelfIndex shelf_idx)
{
    auto result = map_.find(shelf_idx);
    if (result == map_.end())
    {
        LOG(trace) << "UnregisterShelfHeap: mapping not found";
        return false;
    }
    else
    {
        (void)map_.erase(result);
        LOG(trace) << "UnregisterShelfHeap: mapping unregistered";
        return true;
    }
}


ShelfHeap *DistHeap::LookupShelfHeap(ShelfIndex shelf_idx)
{
    auto result = map_.find(shelf_idx);
    if (result == map_.end())
    {
        LOG(trace) << "LookupShelfHeap: mapping not found";
        return NULL;
    }
    else
    {
        LOG(trace) << "LookupShelfHeap: mapping found";
        return result->second;
    }
}

    
ErrorCode DistHeap::OpenShelfHeap(ShelfIndex shelf_idx)
{
    TRACE();
    LOG(trace) << "OpenShelfHeap " << (uint64_t)pool_id_ << "_" << (uint64_t)shelf_idx;
    assert(kMaxShelfCount > shelf_idx);

    ErrorCode ret = NO_ERROR;
    std::string path;
    ret = pool_.GetShelfPath(shelf_idx, path);
    assert(ret == NO_ERROR);
    ShelfHeap *shelf_heap = new ShelfHeap(path, ShelfId(pool_id_, shelf_idx));
    ret = shelf_heap->Open();
    if (ret == NO_ERROR)
    {
        if (RegisterShelfHeap(shelf_idx, shelf_heap) == false)
        {
            LOG(fatal) << "BUG: RegisterShelfHeap failed";
            return BUG;
        }
    }
    else
    {
        LOG(trace) << "OpenShelfHeap " << shelf_idx << " failed (" << ret << ")";
    }
    return ret;    
}

ErrorCode DistHeap::CloseShelfHeap(ShelfIndex shelf_idx)
{
    TRACE();
    LOG(trace) << "CloseShelfHeap " << (uint64_t)pool_id_ << "_" << (uint64_t)shelf_idx;
    assert(kMaxShelfCount > shelf_idx);

    ErrorCode ret = NO_ERROR;
    ShelfHeap *shelf_heap = LookupShelfHeap(shelf_idx);
    if (shelf_heap == NULL)
    {
        LOG(fatal) << "BUG: CloseShelfHeap failed";
        return BUG;
    }
    ret = shelf_heap->Close();
    if (ret == NO_ERROR)
    {
        if (UnregisterShelfHeap(shelf_idx) == false)
        {
            LOG(fatal) << "BUG: UnregisterShelfHeap failed";
            return BUG;
        }
        delete shelf_heap;
    }
    else
    {
        LOG(trace) << "CloseShelfHeap " << shelf_idx << " failed (" << ret << ")";
    }        
    return ret;        
}

ErrorCode DistHeap::RecoverShelfHeap(ShelfIndex shelf_idx)
{
    TRACE();
    LOG(trace) << "RecoverShelfHeap " << (uint64_t)pool_id_ << "_" << (uint64_t)shelf_idx;
    assert(kMaxShelfCount > shelf_idx);

    ErrorCode ret = NO_ERROR;
    std::string path;
    ret = pool_.GetShelfPath(shelf_idx, path);
    assert(ret == NO_ERROR);
    ShelfHeap *shelf_heap = new ShelfHeap(path, ShelfId(pool_id_, shelf_idx));
    ret = shelf_heap->Recover();
    if (ret == NO_ERROR)
    {
        LOG(trace) << "RecoverShelfHeap " << (uint64_t)pool_id_ << "_" << (uint64_t)shelf_idx << " succeeded";
    }
    else
    {
        LOG(trace) << "RecoverShelfHeap " << (uint64_t)pool_id_ << "_" << (uint64_t)shelf_idx << " failed ("
                   << ret << ")"; 
    }
    delete shelf_heap;
    return ret;    
}
    
} // namespace nvmm
