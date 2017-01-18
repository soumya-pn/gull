#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED

#include <gtest/gtest.h>
#include "nvmm/nvmm_fam_atomic.h"

#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"
#include "nvmm/memory_manager.h"

#include "test_common/test.h"

#include "shelf_mgmt/membership.h"
#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/shelf_name.h"

using namespace nvmm;

typedef uint16_t ItemValue;
typedef uint32_t ItemIndex;
typedef MembershipT<ItemValue, ItemIndex> Membership;

static size_t const kShelfSize = 128*1024*1024LLU; // 128 MB
static ShelfId const kShelfId = ShelfId(1,1);
static ItemIndex const kItemCnt = 100;

ShelfName shelf_name;

ErrorCode CreateMembership(ShelfId shelf_id, ItemIndex cnt)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(shelf_id);    
    
    // create the membership array
    ShelfFile shelf(shelf_path.c_str());

    // create a shelf
    EXPECT_EQ(NO_ERROR, shelf.Create(S_IRUSR|S_IWUSR, kShelfSize));    
    EXPECT_TRUE(shelf.Exist());

    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    void *addr;
    size_t size = shelf.Size();
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr));
    Membership membership(addr, size);
    EXPECT_FALSE(membership.Verify());    
    EXPECT_EQ(NO_ERROR, membership.Create(cnt));
    EXPECT_TRUE(membership.Verify());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(addr, size));
    EXPECT_EQ(NO_ERROR, shelf.Close());
    
    return ret;
}


ErrorCode DestroyMembership(ShelfId shelf_id)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(shelf_id);    
    
    // destroy the membership array
    ShelfFile shelf(shelf_path.c_str());
    EXPECT_TRUE(shelf.Exist());

    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    void *addr;
    size_t size = shelf.Size();
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr));
    Membership membership(addr, size);
    EXPECT_TRUE(membership.Verify());
    EXPECT_EQ(NO_ERROR, membership.Destroy());    
    EXPECT_FALSE(membership.Verify());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(addr, size));
    EXPECT_EQ(NO_ERROR, shelf.Close());

    // destroy the shelf
    EXPECT_EQ(NO_ERROR, shelf.Destroy());
    
    return ret;
}


ErrorCode OpenCloseMembership(ShelfId shelf_id)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(shelf_id);    
    
    // open the membership array
    ShelfFile shelf(shelf_path.c_str());    
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    void *addr;
    size_t size = shelf.Size();
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr));
    Membership membership(addr, size);
    EXPECT_EQ(NO_ERROR, membership.Open());    
    // close the membership array
    EXPECT_EQ(NO_ERROR, membership.Close());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(addr, size));
    EXPECT_EQ(NO_ERROR, shelf.Close());

    return ret;
}

ErrorCode AddRemoveSlots(ShelfId shelf_id)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(shelf_id);    

    // open the membership array
    ShelfFile shelf(shelf_path.c_str());    
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    void *addr;
    size_t size = shelf.Size();
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr));
    Membership membership(addr, size);
    EXPECT_EQ(NO_ERROR, membership.Open());    

    // tests START
    bool status;
    ItemValue values[10];

    // GetFreeSlot, MarkSlotUsed
    for (ItemIndex i=0; i<10; i++)
    {
        status = membership.GetFreeSlot(0, values[i]);
        EXPECT_TRUE(status);
        EXPECT_EQ(i+1, values[i]);
    }
    
    for (ItemIndex i=0; i<9; i++)
    {
        status = membership.MarkSlotUsed(0, values[i]);
        EXPECT_FALSE(status);
        EXPECT_EQ(values[9], values[i]);
    }

    status = membership.MarkSlotUsed(0, values[9]);
    EXPECT_TRUE(status);
    EXPECT_EQ(10^(((ItemValue)1)<<(sizeof(ItemValue)*8-1)), values[9]);

    // GetUsedSlot, MarkSlotFree
    status = membership.GetUsedSlot(0, values[0]);
    EXPECT_TRUE(status);
    EXPECT_EQ(values[9], values[0]);

    status = membership.GetUsedSlot(0, values[1]);
    EXPECT_TRUE(status);
    EXPECT_EQ(values[9], values[1]);
    
    status = membership.MarkSlotFree(0, values[0]);
    EXPECT_TRUE(status);
    EXPECT_EQ(10, membership.GetVersionNum(values[0]));

    status = membership.MarkSlotFree(0, values[1]);
    EXPECT_FALSE(status);
    EXPECT_EQ(11, membership.GetVersionNum(values[1]));
    // tests END

    // close the membership array
    EXPECT_EQ(NO_ERROR, membership.Close());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(addr, size));
    EXPECT_EQ(NO_ERROR, shelf.Close());
    
    return ret;
}

ErrorCode FindSlots(ShelfId shelf_id, ItemIndex cnt)
{
    ErrorCode ret = NO_ERROR;
    std::string shelf_path = shelf_name.Path(shelf_id);    
    
    // open the membership array
    ShelfFile shelf(shelf_path.c_str());    
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    void *addr;
    size_t size = shelf.Size();
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, &addr));
    Membership membership(addr, size);
    EXPECT_EQ(NO_ERROR, membership.Open());    

    // tests START
    bool status;
    ItemValue value;
    ItemIndex index;

    for(ItemIndex i=0; i<cnt; i++)
    {
        status = membership.FindFirstFreeSlot(index, 0, cnt-1);
        EXPECT_TRUE(status);
        EXPECT_EQ(i, index);
        EXPECT_EQ(0, membership.GetVersionNumWithIndex(index));
        EXPECT_FALSE(membership.TestValidBitWithIndex(index));
        
        value = membership.GetItemWithIndex(index);
        
        status = membership.MarkSlotUsed(index, value);
        EXPECT_TRUE(status);
        EXPECT_EQ(0, membership.GetVersionNumWithIndex(index));
        EXPECT_TRUE(membership.TestValidBitWithIndex(index));       
    }

    for(ItemIndex i=0; i<cnt; i++)
    {
        status = membership.FindFirstUsedSlot(index, 0, cnt-1);
        EXPECT_TRUE(status);
        EXPECT_EQ(i, index);
        EXPECT_EQ(0, membership.GetVersionNumWithIndex(index));
        EXPECT_TRUE(membership.TestValidBitWithIndex(index));

        value = membership.GetItemWithIndex(index);

        status = membership.MarkSlotFree(index, value);
        EXPECT_TRUE(status);
        EXPECT_EQ(1, membership.GetVersionNumWithIndex(index));
        EXPECT_FALSE(membership.TestValidBitWithIndex(index));
    }    
    // tests END

    // close the membership array
    EXPECT_EQ(NO_ERROR, membership.Close());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(addr, size));
    EXPECT_EQ(NO_ERROR, shelf.Close());
    
    return ret;
}

TEST(Membership, Create)
{
    EXPECT_EQ(NO_ERROR, CreateMembership(kShelfId, kItemCnt));
}

TEST(Membership, OpenClose)
{
    EXPECT_EQ(NO_ERROR, OpenCloseMembership(kShelfId));
}

TEST(Membership, Destroy)
{
    EXPECT_EQ(NO_ERROR, DestroyMembership(kShelfId));
}

TEST(Membership, AddRemoveSlots)
{
    EXPECT_EQ(NO_ERROR, CreateMembership(kShelfId, kItemCnt));
    EXPECT_EQ(NO_ERROR, AddRemoveSlots(kShelfId));    
    EXPECT_EQ(NO_ERROR, DestroyMembership(kShelfId));    
}

TEST(Membership, FindSlots)
{
    EXPECT_EQ(NO_ERROR, CreateMembership(kShelfId, kItemCnt));
    EXPECT_EQ(NO_ERROR, FindSlots(kShelfId, kItemCnt));    
    EXPECT_EQ(NO_ERROR, DestroyMembership(kShelfId));    
}

int main(int argc, char** argv)
{
    InitTest();    
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

