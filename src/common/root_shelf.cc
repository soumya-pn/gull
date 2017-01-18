#include <string>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <boost/filesystem/operations.hpp>

#include "nvmm/error_code.h"

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/log.h"
#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h" // for PoolId

#include "common/common.h"

#include "nvmm/root_shelf.h"

namespace nvmm {

// TODO: clean up redundant code    
RootShelf::RootShelf(std::string pathname)
    : path_{pathname}, fd_{-1}, addr_{nullptr}
{
}

RootShelf::~RootShelf()
{
    if(IsOpen() == true)
    {
        (void)Close();
    }
}

bool RootShelf::Exist()
{
    // TODO: should also check magic number
    boost::filesystem::path shelf_path = boost::filesystem::path(path_.c_str());
    return boost::filesystem::exists(shelf_path);
}

bool RootShelf::IsOpen()
{
    return fd_ != -1;
}

void *RootShelf::Addr()
{
    return (char*)addr_+kCacheLineSize;
}
    
ErrorCode RootShelf::Create()
{
    TRACE();
    if (Exist() == true)
    {
        return SHELF_FILE_FOUND;
    }
    if (IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }
    
    int fd = open(path_.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR|S_IWUSR);
    if (fd == -1)
    {
        LOG(fatal) << "RootShelf: Failed to create the root shelf file " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }
    int ret = ftruncate(fd, kShelfSize);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to truncate the root shelf file " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }
    void *addr = mmap(NULL, kShelfSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
    {
        LOG(fatal) << "RootShelf: Failed to mmap the root shelf file " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }

    //LOG(fatal) << "register fam atomic " << path_ << " " << (uint64_t)addr;    
    ret = fam_atomic_register_region(addr, kShelfSize, fd, 0);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to register fam atomic region " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }        

    // leave space for the magic number
    char *cur = (char*)addr;
    cur+=kCacheLineSize;
    
    // every pool will have a fam spinlock in this root shelf file
    // for multi-process/multi-node
    nvmm_fam_spinlock* array = (nvmm_fam_spinlock*)cur;
    for(int i=0; i<ShelfId::kMaxPoolCount; i++)
    {
        array[i].init();
    }

    // finally set the magic number
    fam_atomic_u64_write((uint64_t*)addr, kMagicNum);
    
    //LOG(fatal) << "unregister fam atomic " << (uint64_t)addr;    
    fam_atomic_unregister_region(addr, kShelfSize);
        
    ret = munmap(addr, kShelfSize);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to unmap the root shelf file " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }

    ret = close(fd);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to close the root shelf file " << path_;
        return SHELF_FILE_CREATE_FAILED;
    }

    return NO_ERROR;
}

ErrorCode RootShelf::Destroy()
{
    TRACE();
    ErrorCode ret = NO_ERROR;
    if (Exist() == false)
    {
        ret = SHELF_FILE_NOT_FOUND;
    }
    if (IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }
    
    boost::filesystem::path shelf_path = boost::filesystem::path(path_.c_str());

    // remove() returns false if the path did not exist in the first place, otherwise true.
    // NOTE: boost::filesystem::remove() is racy at least up to 1.57.0, see
    // https://svn.boost.org/trac/boost/ticket/11166
    // therefore, we try to catch the exception and prevent it from being exposed
    try
    {
        (void)boost::filesystem::remove(shelf_path);
    }
    catch (boost::filesystem::filesystem_error const &err)
    {
        if(err.code().value() == 2)
        {
            LOG(trace) << "boost::filesystem::remove - BUGGY exceptions " << err.code();
        }
        else
        {
            LOG(fatal) << "boost::filesystem::remove - REAL exceptions" << err.code();
            throw (err);
        }
    }
    return ret;
}

ErrorCode RootShelf::Open()
{
    TRACE();
    if(IsOpen() == true)
    {
        return SHELF_FILE_OPENED;
    }

    fd_ = open(path_.c_str(), O_RDWR);
    if (fd_ == -1)
    {
        LOG(fatal) << "RootShelf: Failed to open the root shelf file " << path_;
        return SHELF_FILE_OPEN_FAILED;
    }

    addr_ = mmap(NULL, kShelfSize, PROT_READ|PROT_WRITE, MAP_SHARED, fd_, 0);
    if (addr_ == MAP_FAILED)
    {
        LOG(fatal) << "RootShelf: Failed to mmap the root shelf file " << path_;
        return SHELF_FILE_OPEN_FAILED;
    }

    //LOG(fatal) << "register fam atomic " << path_ << " " << (uint64_t)addr_;    
    int ret = fam_atomic_register_region(addr_, (uint64_t)kShelfSize, fd_, 0);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to register fam atomic region " << path_;
        return SHELF_FILE_OPEN_FAILED;
    }

    uint64_t magic_num = fam_atomic_u64_read((uint64_t*)addr_);
    if (magic_num == kMagicNum)
        return NO_ERROR;
    else
    {
        (void)Close();
        return SHELF_FILE_OPEN_FAILED;
    }
}

ErrorCode RootShelf::Close()
{
    TRACE();
    if(IsOpen() == false)
    {
        return SHELF_FILE_CLOSED;
    }

    //LOG(fatal) << "unregister fam atomic " << (uint64_t)addr_;    
    fam_atomic_unregister_region(addr_, kShelfSize);
            
    int ret = munmap(addr_, kShelfSize);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to unmap the root shelf file " << path_;
        return SHELF_FILE_CLOSE_FAILED;
    }

    ret = close(fd_);
    if (ret == -1)
    {
        LOG(fatal) << "RootShelf: Failed to close the root shelf file " << path_;
        return SHELF_FILE_CLOSE_FAILED;
    }

    fd_ = -1;
    return NO_ERROR;
}

} // namespace nvmm
