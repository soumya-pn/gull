#include <stddef.h>
#include <stdint.h>

#include <string>
#include <assert.h>

#include "nvmm/error_code.h"
#include "nvmm/global_ptr.h"
#include "nvmm/shelf_id.h"
#include "nvmm/heap.h"

#include "nvmm/log.h"

#include "shelf_mgmt/pool.h"

#include "shelf_usage/zone_shelf_heap.h"

#include "allocator/zone_heap.h"

namespace nvmm {
    
ZoneHeap::ZoneHeap(PoolId pool_id)
    : pool_id_{pool_id}, pool_{pool_id}, size_{0}, rmb_{NULL}, is_open_{false}
{
}
    
ZoneHeap::~ZoneHeap()
{
    if(IsOpen() == true)
    {
        (void)Close();
    }    
}

ErrorCode ZoneHeap::Create(size_t shelf_size)
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
            return HEAP_CREATE_FAILED;
        }

        // add one shelf
        ret = pool_.Open(false);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }
        ShelfIndex shelf_idx = kShelfIdx;        
        ret = pool_.AddShelf(shelf_idx,
                             [](ShelfFile *shelf, size_t shelf_size)
                            {
                                ShelfHeap shelf_heap(shelf->GetPath());
                                return shelf_heap.Create(shelf_size);
                            },
                            false
                            );
        if (ret != NO_ERROR)
        {
            (void)pool_.Close(false);
            return HEAP_CREATE_FAILED;
        }
        
        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_CREATE_FAILED;
        }

        return ret;
    }
}

ErrorCode ZoneHeap::Destroy()
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

        // remove the shelf        
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

        std::string path;
        ret = pool_.GetShelfPath(kShelfIdx, path);
        assert(ret == NO_ERROR);        
        ShelfHeap shelf_heap(path, ShelfId(pool_id_, kShelfIdx));
        shelf_heap.Destroy();
        
        ret = pool_.RemoveShelf(kShelfIdx);
        if (ret != NO_ERROR)
        {
            (void)pool_.Close(false);
            return HEAP_DESTROY_FAILED;
        }
        ret = pool_.Close(false);
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        
        // destroy the pool
        ret = pool_.Destroy();
        if (ret != NO_ERROR)
        {
            return HEAP_DESTROY_FAILED;
        }
        return ret;
    }
}

bool ZoneHeap::Exist()
{
    return pool_.Exist();
}

ErrorCode ZoneHeap::Open()
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
    // ret = pool_.Recover();
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Open: Found inconsistency in Region " << (uint64_t)pool_id_;
    // }
    
    // get the shelf
    std::string path;
    ret = pool_.GetShelfPath(kShelfIdx, path);
    if (ret != NO_ERROR)
    {
        return HEAP_OPEN_FAILED;
    }

    // open rmb
    rmb_ = new ShelfHeap(path, ShelfId(pool_id_, kShelfIdx));
    assert (rmb_ != NULL);
    ret = rmb_->Open();
    if (ret != NO_ERROR)
    {
        LOG(error) << "Zone: rmb open failed " << (uint64_t)pool_id_;
        return HEAP_OPEN_FAILED;
    }

    size_ = rmb_->Size();

    is_open_ = true;
    assert(ret == NO_ERROR);    
    return ret;
}

ErrorCode ZoneHeap::Close()
{
    TRACE();
    LOG(trace) << "Close Heap " << (uint64_t)pool_id_;
    assert(IsOpen() == true);
    ErrorCode ret = NO_ERROR;

    // close the rmb
    ret = rmb_->Close();
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }
    delete rmb_;

    // // perform recovery
    // ret = pool_.Recover();
    // if(ret != NO_ERROR)
    // {
    //     LOG(fatal) << "Close: Found inconsistency in Heap " << (uint64_t)pool_id_;
    // }    
    
    // close the pool
    ret = pool_.Close(false);
    if (ret != NO_ERROR)
    {
        return HEAP_CLOSE_FAILED;
    }

    size_ = 0;
    is_open_ = false;
    
    assert(ret == NO_ERROR);        
    return ret;
}

size_t ZoneHeap::Size()
{
    assert(IsOpen() == true);
    return size_;
}

GlobalPtr ZoneHeap::Alloc (size_t size)
{
    assert(IsOpen() == true);
    GlobalPtr ptr;
    Offset offset = rmb_->Alloc(size);
    if (rmb_->IsValidOffset(offset) == true)
    {
        // this offset has size encoded
        ptr = GlobalPtr(ShelfId(pool_id_, kShelfIdx), offset);
    }
    return ptr;
}
    
void ZoneHeap::Free (GlobalPtr global_ptr)
{
    assert(IsOpen() == true);
    Offset offset = global_ptr.GetReserveAndOffset();
    rmb_->Free(offset);
}

void *ZoneHeap::GlobalToLocal(GlobalPtr global_ptr)
{
    TRACE();
    assert(IsOpen() == true);
    void *local_ptr = NULL;
    Offset offset = global_ptr.GetReserveAndOffset();
    local_ptr = rmb_->OffsetToPtr(offset);
    return local_ptr;
}
    
} // namespace nvmm
