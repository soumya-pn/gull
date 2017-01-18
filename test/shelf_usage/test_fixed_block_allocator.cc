#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED

#include <gtest/gtest.h>
#include <fam_atomic.h>

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"

#include "common/common.h"
#include "test_common/test.h"

#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/shelf_name.h"

#include "shelf_usage/fixed_block_allocator.h"

using namespace nvmm;

static size_t const kShelfSize = 128*1024*1024LLU; // 128 MB
static size_t const block_size = kCacheLineSize;

TEST(FixedBlockAllocator, Test)
{
    ShelfName shelf_name;
    ShelfId const shelf_id(1);
    std::string path = shelf_name.Path(shelf_id);
    ShelfFile shelf(path);

    EXPECT_EQ(NO_ERROR, shelf.Create(S_IRUSR|S_IWUSR, kShelfSize));

    void* address = NULL;
    
    // mmap
    size_t length = kShelfSize/2;
    loff_t offset = kShelfSize/2;
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, kShelfSize, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&address));

    // create the allocator
    FixedBlockAllocator fba((char*)address+offset, block_size, 0, 0, length);

    //std::cout << "size " << fba.size() << std::endl;    
    //std::cout << "user_metadata_size " << fba.user_metadata_size() << std::endl;
    //std::cout << "max_blocks " << fba.max_blocks() << std::endl;

    Offset *ptr = new Offset[fba.max_blocks()];
    for (int i = 0; i< fba.max_blocks(); i++)
    {
        ptr[i] = fba.alloc();
        //std::cout << "ptr[" << i << "] " << ptr[i] << " " << (uint64_t)fba.from_Offset(ptr[i]) << std::endl;
    }       
    EXPECT_EQ(0, fba.alloc());
    
    for (int i = 0; i< fba.max_blocks(); i++)
    {
        fba.free(ptr[i]);
    }       

    for (int i = 0; i< fba.max_blocks(); i++)
    {
        ptr[i] = fba.alloc();
        //std::cout << "ptr[" << i << "] " << ptr[i] << " " << (uint64_t)fba.from_Offset(ptr[i]) << std::endl;
    }       
    EXPECT_EQ(0, fba.alloc());

    // unmap
    EXPECT_EQ(NO_ERROR, shelf.Unmap(address, kShelfSize));
    EXPECT_EQ(NO_ERROR, shelf.Close());        

    
    EXPECT_EQ(NO_ERROR, shelf.Destroy());    
}

int main(int argc, char** argv)
{
    InitTest();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

