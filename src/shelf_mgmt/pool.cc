#include <stddef.h>
#include <stdint.h>
#include <string>
#include <functional>
#include <pthread.h>

#include <assert.h>
#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED
#include <random>
#include <limits>
#include <boost/filesystem.hpp>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/log.h"

#include "common/common.h"

#include "shelf_mgmt/shelf_name.h"
#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/membership.h"

#include "shelf_mgmt/pool.h"

namespace nvmm {

// TODO: make SHELF_BASE_DIR and this file prefix string configurable in MemoryManager
// right now SHELF_BASE_DIR is defined in the root CMakeLists.txt
ShelfName Pool::shelf_name_=ShelfName(SHELF_BASE_DIR, "NVMM_Shelf");

Pool::Pool(PoolId pool_id)
    : pool_id_{pool_id}, is_open_{false},
      metadata_shelf_{shelf_name_.Path(ShelfId(kMetadataPoolId, (ShelfIndex)pool_id))},
      addr_{NULL},
      membership_{NULL}
{
    assert(pool_id_ < kMaxPoolCount);
    int ret = pthread_rwlock_init(&rwlock_, NULL);
    assert(ret == 0);
}

Pool::~Pool()
{    
}

ErrorCode Pool::Create(size_t shelf_size)
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (Exist() == true)
    {
        return POOL_FOUND;
    }
    if (IsOpen() == true)
    {
        return POOL_OPENED;
    }
    // TODO: enable this check later
    // if (shelf_size % kShelfSize !=0)
    // {
    //     return POOL_CREATE_FAILED;
    // }
    
    ret = metadata_shelf_.Create(S_IRUSR | S_IWUSR, kMetadataShelfSize);
    if (ret != NO_ERROR)
    {
        if (ret == SHELF_FILE_FOUND)
            return POOL_FOUND;
        else
            return POOL_CREATE_FAILED;
    }

    ret = OpenMapMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_CREATE_FAILED;
    }    

    // set shelf_size
    fam_atomic_u64_write((uint64_t*)addr_, (uint64_t)shelf_size);

    // init membership
    Membership membership((char*)addr_+kCacheLineSize, size_-kCacheLineSize);
    ret = membership.Create(kMaxShelfCount);
    if (ret != NO_ERROR)
    {
        return POOL_CREATE_FAILED;
    }

    ret = UnmapCloseMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_CREATE_FAILED;
    }    
    
    return ret;
}

ErrorCode Pool::Destroy()
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (Exist() == false)
    {
        return POOL_NOT_FOUND;
    }
    if (IsOpen() == true)
    {
        return POOL_OPENED;
    }
    // delete all shelves
    ret = Open(false);
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }
    ret = Recover();
    if(ret != NO_ERROR)
    {
        LOG(fatal) << "Found inconsistency in pool " << (uint64_t)pool_id_;
    }        
    for (ShelfIndex shelf_idx = 0; shelf_idx < Size(); shelf_idx++)
    {
        if (CheckShelf(shelf_idx) == true)
        {
            ret = RemoveShelf(shelf_idx);
            if (ret != NO_ERROR)
            {
                (void)Close(false);
                return POOL_DESTROY_FAILED;
            }
        }
    }
    ret = Close(false);
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }

    // destroy the membership
    ret = OpenMapMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }    

    fam_atomic_u64_write((uint64_t*)addr_, (uint64_t)0);
    
    Membership membership((char*)addr_+kCacheLineSize, size_-kCacheLineSize);
    ret = membership.Destroy();
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }

    ret = UnmapCloseMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }    
    
    // delete the metadata shelf
    ret = metadata_shelf_.Destroy();
    if (ret != NO_ERROR)
    {
        return POOL_DESTROY_FAILED;
    }

    return ret;
}

bool Pool::Exist()
{
    return metadata_shelf_.Exist();
}

bool Pool::Verify()
{
    TRACE();
    if (Exist() == false)
    {
        return POOL_NOT_FOUND;
    }
    if (IsOpen() == true)
    {
        return POOL_OPENED;
    }
    
    ErrorCode ret = NO_ERROR;
    bool status = false;

    ret = OpenMapMetadataShelf();
    if (ret != NO_ERROR)
    {
        status = false;        
    }    
    else
    {
        Membership membership((char*)addr_+kCacheLineSize, size_-kCacheLineSize);    
        status = membership.Verify();

        ret = UnmapCloseMetadataShelf();
        if (ret != NO_ERROR)
        {
            status = false;
        }    
    }

    if (status == false)
    {
        if (ret == SHELF_FILE_INVALID_FORMAT)
        {   
            return POOL_INVALID_META_FILE;
        }
        else
        {
            LOG(fatal) << "Pool::Verify(): something else went wrong...";
            assert(0);
        }
    }

    return ret;
}
    
ErrorCode Pool::Open(bool recover)
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (IsOpen() == true)
    {
        return POOL_OPENED;
    }
    if (Exist() == false)
    {
        return POOL_NOT_FOUND;
    }

    ret = OpenMapMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_OPEN_FAILED;
    }    

    // load shelf_size
    shelf_size_ = fam_atomic_u64_read((uint64_t*)addr_);

    // load membership
    membership_ = new Membership((char*)addr_+kCacheLineSize, size_-kCacheLineSize);
    ret = membership_->Open();
    if (ret != NO_ERROR)
    {
        return POOL_OPEN_FAILED;
    }

    is_open_ = true;    
            
    // do recovery
    // TODO: need a way to tell if a crash occured or it was a clean shutdown
    if (recover==true)
    {
        if(Recover() != NO_ERROR)
        {
            LOG(fatal) << "Found inconsistency in pool " << pool_id_;
        }
    }

    return ret;
}

ErrorCode Pool::Close(bool recover)
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    // perform recovery
    // other processes may have crashed and did not restart (no recovery)
    if (recover==true)
    {
        if (Recover() != NO_ERROR)
        {
            LOG(fatal) << "Found inconsistency in pool " << pool_id_;
        }
    }   
    // close the bitmap
    ret = membership_->Close();
    if (ret != NO_ERROR)
    {
        return POOL_CLOSE_FAILED;
    }

    delete membership_;
    membership_ = NULL;
        
    ret = UnmapCloseMetadataShelf();
    if (ret != NO_ERROR)
    {
        return POOL_CLOSE_FAILED;
    }    
    
    is_open_ = false;
    return ret;
}

// RETURN:
// - NO_ERROR: no inconsistency was found
// - POOL_INCONSISTENCY_FOUND: there was inconsistency
// NOTE:
// - Only when we are the only process accessing the pool, POOL_INCONSISTENCY_FOUND means there was
// indeed something wrong. When there are multipl processess accessing the pool,
// POOL_INCONSISTENCY_FOUND could mean that we saw an intermediate inconsistent state due to other
// processes's on-going operations
// - TODO: It is potentially racy with concurrent format operations (e.g., deleting a file
// that is being formatted). 
ErrorCode Pool::Recover()
{
    TRACE();
    ErrorCode ret = NO_ERROR;

    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }

    Version value, version;
    bool status;

    for (ShelfIndex shelf_idx = 0; shelf_idx < kMaxShelfCount; shelf_idx++)
    {
        ShelfId shelf_id = ShelfId(pool_id_, shelf_idx);
        // clean up old versions
        if (membership_->TestValidBitWithIndex(shelf_idx, value) == true)
        {
            // valid bit is 1
            version = membership_->GetVersionNum(value);
            assert(version > (Version)0);
            
            // remove previous versions or tmp versions
            // TODO: potentially racy with shelf format (e.g., truncate)
            status = RemoveOldShelfFiles(pool_id_, shelf_idx, version);
            if (status == true)
            {
                LOG(trace) << "Recover: deleted old version for shelf index " << (uint64_t)shelf_idx;
            }

            std::string path = shelf_name_.Path(shelf_id, std::to_string(version), "");
            ShelfFile shelf(path);
            status = shelf.Exist();
            if (status == false)
            {
                ret = POOL_INCONSISTENCY_FOUND;
                // this could happen when there is an on-going AddShelf(shelf_idx)
                LOG(trace) << "Recover: found potential inconsistency for shelf index " << (uint64_t)shelf_idx << "; valid==1 but file does not exist";
            }
        }
        else
        {
            // valid bit is 0
            version = membership_->GetVersionNum(value);

            if (version == 0)
            {
                // this slot has never been used
                continue;
            }
            
            // remove previous versions or tmp versions
            // TODO: potentially racy with shelf format (e.g., truncate)
            status = RemoveOldShelfFiles(pool_id_, shelf_idx, version);
            if (status == true)
            {
                LOG(trace) << "Recover: deleted old version for shelf index " << (uint64_t)shelf_idx;
            }

            std::string path = shelf_name_.Path(shelf_id, std::to_string(version), "");
            ShelfFile shelf(path);
            status = shelf.Exist();
            if (status == true)
            {
                ret = POOL_INCONSISTENCY_FOUND;
                // this could happen when there is an on-going RemoveShelf(shelf_idx)
                LOG(trace) << "Recover: found inconsistency for shelf index " << (uint64_t)shelf_idx << "; valid==0 but file exists";
            }
        }
    }
    
    return ret;
}

ErrorCode Pool::TruncateShelfFile(ShelfFile *shelf, size_t shelf_size)
{
    assert(shelf != NULL);
    if (shelf->Exist() == false)
    {
        return SHELF_FILE_NOT_FOUND;
    }

    ErrorCode ret = NO_ERROR;
    ret = shelf->Open(O_RDWR);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    ret = shelf->Truncate(shelf_size);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    ret = shelf->Close();
    if (ret != NO_ERROR)
    {
        return ret;
    }

    return ret;
}
    
ErrorCode Pool::NewShelf(ShelfIndex &shelf_idx)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    return NewShelf(shelf_idx,
                    [](ShelfFile* shelf, size_t size)
                    {                        
                        return TruncateShelfFile(shelf, size);
                    }
                    );
}

ErrorCode Pool::AddShelf(ShelfIndex &shelf_idx, bool assign_diff_shelf_idx)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    return AddShelf(shelf_idx,
                    [](ShelfFile* shelf, size_t size)
                    {
                        return TruncateShelfFile(shelf, size);
                    },
                    assign_diff_shelf_idx
                    );                    
}

// Returns:
// - NO_ERROR: the shelf now exists, and the shelf was created by us; the actual assigned shelf_idx
// is returned 
// - POOL_ADD_SHELF_FAILED: failed to add the shelf 
ErrorCode Pool::NewShelf(ShelfIndex &shelf_idx, FormatFn format_func)
{
    TRACE();
    shelf_idx = (ShelfIndex)0;
    return AddShelf(shelf_idx, format_func);
}

// Returns:
// - NO_ERROR: the shelf now exists, and the shelf was created by us; the actual assigned shelf_idx
// is returned 
// - POOL_ADD_SHELF_FAILED: failed to add the shelf 
ErrorCode Pool::AddShelf(ShelfIndex &shelf_idx, FormatFn format_func, bool assign_diff_shelf_idx)
{
    TRACE();
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    assert(kMaxShelfCount > shelf_idx);

    ErrorCode ret = NO_ERROR;

    // STEP 1:
    // create a temp file with a random version number and a suffix of "add"
    // format this file
    // assuming shelf creation is atomic
    ShelfId tmp_shelf_id = ShelfId(pool_id_, shelf_idx);
    Version tmp_version = RandForAddShelf();
    std::string tmp_suffix = "add";
    std::string tmp_path = shelf_name_.Path(tmp_shelf_id, std::to_string(tmp_version), tmp_suffix);
    ShelfFile shelf(tmp_path);
    
    // create empty shelf file
    while(1)
    {
        ret = shelf.Create(S_IRUSR | S_IWUSR);
        if (ret == SHELF_FILE_FOUND)
        {
            tmp_version = RandForAddShelf();
            tmp_path = shelf_name_.Path(tmp_shelf_id, std::to_string(tmp_version), tmp_suffix);            
            shelf = ShelfFile(tmp_path);
        }
        else
        {
            break;
        }
    }
    if (ret != NO_ERROR)
    {
        LOG(error) << "AddShelf failed at Create " << (uint64_t)shelf_idx;
        return POOL_ADD_SHELF_FAILED;
    }

    // format the shelf
    if (format_func == nullptr)
    {
        ret = DefaultFormatFn(&shelf, shelf_size_);
    }
    else
    {
        ret = format_func(&shelf, shelf_size_);
    }
    if (ret != NO_ERROR)
    {
        LOG(error) << "AddShelf failed at Format " << (uint64_t)shelf_idx;
        return POOL_ADD_SHELF_FAILED;
    }

    // STEP 2:
    // assign a shelf_idx to this shelf
    Version actual_value, expected_value;
    Version actual_version, expected_version;
    ShelfId actual_shelf_id;
    std::string actual_suffix = "";
    std::string actual_path;

    if (assign_diff_shelf_idx == false)
    {
        // use the specified shelf index
        actual_shelf_id = ShelfId(pool_id_, shelf_idx);

        // try to assign shelf_idx to this shelf
        if (membership_->GetFreeSlot(shelf_idx, expected_value) == true)
        {
            expected_version = membership_->GetVersionNum(expected_value);        
            actual_value = expected_value;

            // rename the shelf from tmp version to expected version, and remove the suffix
            actual_path = shelf_name_.Path(actual_shelf_id, std::to_string(expected_version), actual_suffix);
            ret = shelf.Rename(actual_path.c_str());
            if (ret != NO_ERROR)
            {
                LOG(trace) << "AddShelf: there must be an on-going Recover()";
                return POOL_ADD_SHELF_FAILED;
            }

            // add it to the membership bitmap
            if (membership_->MarkSlotUsed(shelf_idx, actual_value) == true)
            {
                actual_version = membership_->GetVersionNum(actual_value);
                assert(membership_->TestValidBit(actual_value) == true);
                LOG(trace) << "AddShelf succeeded " << (uint64_t)shelf_idx
                           <<"(ver " << actual_version << ")";
                ret = NO_ERROR;
                return ret;
            }
        }
        return POOL_ADD_SHELF_FAILED;
    }

    ShelfIndex start_idx = shelf_idx;
    ShelfIndex end_idx = (ShelfIndex)(shelf_idx+kMaxShelfCount-1);    
    while (start_idx <= end_idx &&
           membership_->FindFirstFreeSlot(shelf_idx, start_idx, end_idx) == true)
    {
        LOG(trace) << "AddShelf try to assign " << (uint64_t)shelf_idx;        
        actual_shelf_id = ShelfId(pool_id_, shelf_idx);

        // try to assign shelf_idx to this shelf
        if (membership_->GetFreeSlot(shelf_idx, expected_value) == true)
        {
            expected_version = membership_->GetVersionNum(expected_value);        
            actual_value = expected_value;

            // rename the shelf from tmp version to expected version, and remove the suffix
            actual_path = shelf_name_.Path(actual_shelf_id, std::to_string(expected_version), actual_suffix);
            ret = shelf.Rename(actual_path.c_str());
            if (ret != NO_ERROR)
            {
                LOG(trace) << "AddShelf: there must be an on-going Recover()";
                return POOL_ADD_SHELF_FAILED;
            }
        
            // add it to the membership bitmap
            if (membership_->MarkSlotUsed(shelf_idx, actual_value) == true)
            {
                actual_version = membership_->GetVersionNum(actual_value);
                assert(membership_->TestValidBit(actual_value) == true);
                LOG(trace) << "AddShelf succeeded " << (uint64_t)shelf_idx
                           <<"(ver " << actual_version << ")";
                ret = NO_ERROR;
                return ret;
            }
        }

        // someone is competing with us
        // go find the next free shelf index
        start_idx = (ShelfIndex)(shelf_idx + 1);
    }
    
    LOG(error) << "Cannot add this shelf...";
    ret = POOL_ADD_SHELF_FAILED;
    return ret;
}


// Returns:
// - NO_ERROR: the shelf index is gone, and it was removed by us
// - POOL_SHELF_NOT_FOUND: the shelf index is gone, but it was not removed by us
// - POOL_REMOVE_SHELF_FAILED: the shelf index is in use  the shelf now has a new version
ErrorCode Pool::RemoveShelf(ShelfIndex shelf_idx)
{
    TRACE();
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    assert(kMaxShelfCount > shelf_idx);

    ErrorCode ret = NO_ERROR;

    // STEP 1:
    // mark this shelf index free
    Version actual_value;
    Version actual_version;
    ShelfId shelf_id = ShelfId(pool_id_, shelf_idx);
    if (membership_->MarkSlotFree(shelf_idx, actual_value) == true)
    {
        actual_version = membership_->GetVersionNum(actual_value);
        assert(membership_->TestValidBit(actual_value) == true);

        // TODO: use rename and spawn a thread to do the deletion         
        // remove the shelf file
        // assuming shelf deletion is atomic
        std::string path = shelf_name_.Path(shelf_id, std::to_string(actual_version), "");
        ShelfFile shelf(path);
        ret = shelf.Destroy();
        if (ret != NO_ERROR)
        {
            if (ret == SHELF_FILE_NOT_FOUND)
            {
                LOG(trace) << "RemoveShelf: there must be an on-going Recover()";                
            }
            else
            {
                LOG(fatal) << "BUG RemoveShelf";
                assert(0);
            }
        }

        LOG(trace) << "RemoveShelf succeeded " << (uint64_t)shelf_idx
                   <<"(ver " << actual_version << ")";
        ret = NO_ERROR;
    }
    else
    {
        actual_version = membership_->GetVersionNum(actual_value);

        if(membership_->TestValidBit(actual_value) == true)
        {
            // soneone else must have removed the version we saw and have created a new version
            LOG(error) << "There is a new version of this shelf " << (uint64_t)shelf_idx
                       <<"(ver " << actual_version << ")";
            ret = POOL_REMOVE_SHELF_FAILED;
        }
        else
        {
            // this shelf is already free or
            // soneone else must have started removing the version we saw or be in the process of
            // creating a new version
            LOG(error) << "Someone beat us " << (uint64_t)shelf_idx
                       <<"(ver " << actual_version << ")";
            ret = POOL_SHELF_NOT_FOUND;
        }
    }
    return ret;
}

bool Pool::FindNextShelf(ShelfIndex &shelf_idx, ShelfIndex start_idx, ShelfIndex end_idx)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    return membership_->FindFirstUsedSlot(shelf_idx, start_idx, end_idx);
}
    
bool Pool::CheckShelf(ShelfIndex shelf_idx)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    return membership_->TestValidBitWithIndex(shelf_idx);
}

ErrorCode Pool::GetShelfId(ShelfIndex shelf_idx, ShelfId &shelf_id)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    if (membership_->TestValidBitWithIndex(shelf_idx) == true)
    {
        shelf_id = ShelfId(pool_id_, shelf_idx);
        return NO_ERROR;        
    }
    else
    {
        return POOL_SHELF_NOT_FOUND;
    }
}

ErrorCode Pool::GetShelfIdx(ShelfId shelf_id, ShelfIndex &shelf_idx)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    PoolId pool_id = shelf_id.GetPoolId();
    if (pool_id != pool_id_)
    {
        return POOL_INVALID_POOL_ID;
    }
    ShelfIndex idx = shelf_id.GetShelfIndex();    
    if (membership_->TestValidBitWithIndex(idx) == true)
    {
        shelf_idx = idx;
        return NO_ERROR;        
    }
    else
    {
        return POOL_SHELF_NOT_FOUND;
    }    
}
    
ErrorCode Pool::GetShelfPath(ShelfIndex shelf_idx, std::string &shelf_path)
{
    if (IsOpen() == false)
    {
        return POOL_CLOSED;
    }
    Version value;
    if (membership_->TestValidBitWithIndex(shelf_idx, value) == true)
    {
        ShelfId shelf_id = ShelfId(pool_id_, shelf_idx);                        
        Version version = membership_->GetVersionNum(value);
        shelf_path = shelf_name_.Path(shelf_id,
                                      std::to_string(version));
        return NO_ERROR;        
    }
    else
    {
        return POOL_SHELF_NOT_FOUND;
    }    
}

// random number generator for AddShelf
Pool::Version Pool::RandForAddShelf()
{
    std::random_device r;
    std::default_random_engine e1(r());
    std::uniform_int_distribution<Version> uniform_dist(std::numeric_limits<Version>::min(),
                                                        std::numeric_limits<Version>::max());
    return uniform_dist(e1);
}
            
// helper function for Pool::Recover()
// TODO: need more optimization
bool Pool::RemoveOldShelfFiles(PoolId pool_id_, ShelfIndex shelf_idx, Version version)
{        
    bool found_old_version = false;

    ShelfId shelf_id = ShelfId(pool_id_, shelf_idx);    
    std::string prefix = shelf_name_.Path(shelf_id) + "_";
    std::string const suffix = "add";

    boost::filesystem::path base_path = boost::filesystem::path(SHELF_BASE_DIR);
    boost::filesystem::directory_iterator end_iter;
    for (boost::filesystem::directory_iterator dir_itr(base_path);
         dir_itr != end_iter;
         dir_itr++)
    {
        std::string pathname = std::string(dir_itr->path().string());
        if (std::equal(prefix.begin(),
                       prefix.begin()+std::min(pathname.size(), prefix.size()),
                       pathname.begin()) == true)
        {
            // pathname has the prefix
            std::string version_string = pathname.substr(prefix.size(), std::string::npos);
            Version old_version = (Version)std::stoul(version_string);
            
            if(version_string.size() > suffix.size() &&
               version_string.substr(version_string.size() - suffix.size(), std::string::npos) == suffix)
            {
                // delete tmp versions
                LOG(trace) << "RemoveOldShelfFiles: found TMP version " << shelf_id << " " << old_version;
                std::string path = shelf_name_.Path(shelf_id, std::to_string(old_version), suffix);
                ShelfFile shelf(path);
                (void)shelf.Destroy();
                found_old_version = true;
            }
            else
            {
                // delete old versions
                if (old_version < version)
                {
                    LOG(trace) << "RemoveOldShelfFiles: found OLD version " << shelf_id << " " << old_version;
                    std::string path = shelf_name_.Path(shelf_id, std::to_string(old_version), "");
                    ShelfFile shelf(path);
                    (void)shelf.Destroy();
                    found_old_version = true;
                }
            }
        }
    }
    return found_old_version;
}
        
ErrorCode Pool::DefaultFormatFn(ShelfFile *shelf, size_t shelf_size)
{
    assert(shelf != NULL);
    if (shelf->Exist() == true)
    {
        return NO_ERROR;
    }
    else
    {
        return SHELF_FILE_NOT_FOUND;
    }
}

ErrorCode Pool::OpenMapMetadataShelf()
{
    ErrorCode ret = NO_ERROR;

    ret = metadata_shelf_.Open(O_RDWR);
    if (ret != NO_ERROR)
    {
        return ret;
    }

    size_ = metadata_shelf_.Size();
    ret = metadata_shelf_.Map(NULL, size_, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr_, true);
    if (ret != NO_ERROR)
    {
        return ret;
    }
          
    return ret;
}

ErrorCode Pool::UnmapCloseMetadataShelf()
{
    ErrorCode ret = NO_ERROR;

    ret = metadata_shelf_.Unmap(addr_, size_, true);
    if (ret != NO_ERROR)
    {
        return ret;
    }
    addr_ = NULL;
    size_ = 0;
    
    ret = metadata_shelf_.Close();
    if (ret != NO_ERROR)
    {
        return ret;
    }
        
    return ret;
}
    
} // namespace nvmm
