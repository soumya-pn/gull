#include <sys/wait.h>
#include <signal.h>
#include <random>
#include <gtest/gtest.h>

#include "test_common/test.h"

#include "nvmm/memory_manager.h"
#include "shelf_mgmt/pool.h"
#include "shelf_mgmt/shelf_name.h"

using namespace nvmm;


std::random_device r;
std::default_random_engine e1(r());
uint32_t rand_uint32(uint32_t min, uint32_t max)
{
    std::uniform_int_distribution<uint32_t> uniform_dist(min, max);
    return uniform_dist(e1);
}


// functional test cases
TEST(Pool, CreateDestroyExist)
{
    PoolId const pool_id = 1;
    Pool pool(pool_id);
    
    EXPECT_FALSE(pool.Exist());
    EXPECT_EQ(NO_ERROR, pool.Create());
    EXPECT_TRUE(pool.Exist());
    EXPECT_EQ(POOL_FOUND, pool.Create());
    EXPECT_EQ(NO_ERROR, pool.Destroy());
    EXPECT_FALSE(pool.Exist());
    EXPECT_EQ(POOL_NOT_FOUND, pool.Destroy());
}

TEST(Pool, OpenClose)
{
    PoolId const pool_id = 1;
    Pool pool(pool_id);
    ShelfIndex shelf_idx = pool.Size();

    EXPECT_EQ(POOL_NOT_FOUND, pool.Open(true));    
    EXPECT_EQ(NO_ERROR, pool.Create());
    EXPECT_EQ(NO_ERROR, pool.Open(true));
    EXPECT_FALSE(pool.FindNextShelf(shelf_idx, pool.Size())); // this pool is still empty    
    EXPECT_EQ(NO_ERROR, pool.Close(false));    
    EXPECT_EQ(NO_ERROR, pool.Open(true));
    EXPECT_FALSE(pool.FindNextShelf(shelf_idx, pool.Size())); // this pool is still empty
    EXPECT_EQ(NO_ERROR, pool.Close(false));    
    EXPECT_EQ(NO_ERROR, pool.Destroy());
}   

TEST(Pool, ShelfManagement)
{
    PoolId const pool_id = 1;
    Pool pool(pool_id);
    ShelfIndex shelf_idx = pool.Size();

    EXPECT_EQ(NO_ERROR, pool.Create());

    // first open
    EXPECT_EQ(NO_ERROR, pool.Open(true));
    EXPECT_FALSE(pool.FindNextShelf(shelf_idx, pool.Size())); // this pool is still empty

    // allocate a new shelf (0)
    EXPECT_EQ(NO_ERROR, pool.NewShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)0, shelf_idx);
    EXPECT_EQ(true, pool.CheckShelf(shelf_idx));

    // allocate another new shelf (1)
    EXPECT_EQ(NO_ERROR, pool.NewShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)1, shelf_idx);
    EXPECT_EQ(true, pool.CheckShelf(shelf_idx));
    EXPECT_EQ(true, pool.FindNextShelf(shelf_idx, 2, 0));
    EXPECT_EQ((ShelfIndex)0, shelf_idx);

    // allocate a new shelf with a specified shelf index (2)
    shelf_idx = 2;
    EXPECT_EQ(NO_ERROR, pool.AddShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)2, shelf_idx);
    EXPECT_EQ(true, pool.CheckShelf(shelf_idx));

    // allocate a new shelf with an existing shelf index (2)
    EXPECT_EQ(NO_ERROR, pool.AddShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)3, shelf_idx);
   
    // remove shelf 0
    EXPECT_EQ(NO_ERROR, pool.RemoveShelf(0));
    
    // remove shelf 0 again
    EXPECT_EQ(POOL_SHELF_NOT_FOUND, pool.RemoveShelf(0));

    EXPECT_EQ(NO_ERROR, pool.Recover());    
    EXPECT_EQ(NO_ERROR, pool.Close(false));    

    // second open
    EXPECT_EQ(NO_ERROR, pool.Open(true));

    EXPECT_FALSE(pool.CheckShelf(0));    
    EXPECT_TRUE(pool.CheckShelf(1));
    EXPECT_TRUE(pool.CheckShelf(2));

    // the first available shelf is 1
    EXPECT_TRUE(pool.FindNextShelf(shelf_idx, pool.Size()));
    EXPECT_EQ((ShelfIndex)1, shelf_idx);

    // allocate a new shelf (0)
    EXPECT_EQ(NO_ERROR, pool.NewShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)0, shelf_idx);
    EXPECT_EQ(true, pool.CheckShelf(shelf_idx));

    EXPECT_EQ(NO_ERROR, pool.Recover());    
    EXPECT_EQ(NO_ERROR, pool.Close(false));    

    EXPECT_EQ(NO_ERROR, pool.Destroy());
}   

TEST(Pool, ShelfUsage)
{
    PoolId const pool_id = 1;
    Pool pool(pool_id);
    ShelfIndex shelf_idx = pool.Size();

    EXPECT_EQ(NO_ERROR, pool.Create());

    EXPECT_EQ(NO_ERROR, pool.Open(true));

    // invalid shelf index    
    ShelfId shelf_id;
    std::string shelf_path;

    EXPECT_EQ(POOL_SHELF_NOT_FOUND, pool.GetShelfId(0, shelf_id));
    EXPECT_EQ(POOL_SHELF_NOT_FOUND, pool.GetShelfPath(0, shelf_path));

    shelf_id = ShelfId(pool_id, 0);
    //    EXPECT_EQ(ShelfId(0x100), shelf_id);
    EXPECT_EQ(POOL_SHELF_NOT_FOUND, pool.GetShelfIdx(shelf_id, shelf_idx));

    // invalid pool id    
    shelf_id = ShelfId(2, 0);
    //EXPECT_EQ(ShelfId(0x200), shelf_id);
    EXPECT_EQ(POOL_INVALID_POOL_ID, pool.GetShelfIdx(shelf_id, shelf_idx));
    
    // allocate a new shelf (0)
    EXPECT_EQ(NO_ERROR, pool.NewShelf(shelf_idx));
    EXPECT_EQ((ShelfIndex)0, shelf_idx);
    EXPECT_EQ(true, pool.CheckShelf(shelf_idx));

    // valid shelf index
    EXPECT_EQ(NO_ERROR, pool.GetShelfId(0, shelf_id));
    EXPECT_EQ(NO_ERROR, pool.GetShelfPath(0, shelf_path));

    shelf_id = ShelfId(pool_id, 0);
    //EXPECT_EQ(ShelfId(0x100), shelf_id);
    EXPECT_EQ(NO_ERROR, pool.GetShelfIdx(shelf_id, shelf_idx));
    EXPECT_EQ((ShelfIndex)0, shelf_idx);
    
    // invalid pool id    
    shelf_id = ShelfId(2, 0);
    //EXPECT_EQ(ShelfId(0x200), shelf_id);
    EXPECT_EQ(POOL_INVALID_POOL_ID, pool.GetShelfIdx(shelf_id, shelf_idx));

    EXPECT_EQ(NO_ERROR, pool.Recover());     
    EXPECT_EQ(NO_ERROR, pool.Close(false));    
    
    EXPECT_EQ(NO_ERROR, pool.Destroy());
}   


// TODO: disable this test if we run it in VM
#ifndef LFS
// single-process multi-threaded test
// slow
struct thread_argument{
    int  id;
    Pool *pool;
    int count;
};

void *worker(void *thread_arg)
{
    thread_argument *arg = (thread_argument*)thread_arg;
    ErrorCode ret = NO_ERROR;
    bool state = false;
    
    int count = arg->count;
    assert(count != 0);
    Pool *pool = arg->pool;
    assert(pool != NULL);
    
    for (int i=0; i<count; i++)
    {
        ShelfIndex target_shelf_idx = (ShelfIndex)rand_uint32(0, pool->Size()-1);
        pool->ReadLock();
        //std::cout << "Thread " << arg->id << " checkshelf " << target_shelf_idx << std::endl;
        state = pool->CheckShelf(target_shelf_idx);
        pool->ReadUnlock();

        if (state == false)
        {
            pool->WriteLock();
            //std::cout << "Thread " << arg->id << " addshelf" << std::endl;
            ret = pool->AddShelf(target_shelf_idx);
            pool->WriteUnlock();
            if (ret == NO_ERROR)
            {
                goto done;
            }
        }

        ShelfIndex actual_shelf_idx;
        
        pool->WriteLock();
        //std::cout << "Thread " << arg->id << " newshelf" << std::endl;
        ret = pool->NewShelf(actual_shelf_idx);
        if (ret == POOL_MEMBERSHIP_FULL)
        {
            //std::cout << "Thread " << arg->id << " POOL_MEMBERSHIP_FULL find remove new shelf" << std::endl;
            EXPECT_TRUE(pool->FindNextShelf(actual_shelf_idx, (ShelfIndex)(target_shelf_idx+1)));
            EXPECT_EQ(NO_ERROR, pool->RemoveShelf(actual_shelf_idx));
            ShelfIndex new_shelf_idx;
            EXPECT_EQ(NO_ERROR, pool->NewShelf(new_shelf_idx));
            EXPECT_EQ(new_shelf_idx, actual_shelf_idx);
        }
        pool->WriteUnlock();

    done:
        pool->WriteLock();
        ShelfIndex new_shelf_idx;
        //std::cout << "Thread " << arg->id << " done" << std::endl;
        state = pool->FindNextShelf(new_shelf_idx, (ShelfIndex)(target_shelf_idx+1));
        if (state == true)
        {
            ret = pool->RemoveShelf(new_shelf_idx);
            EXPECT_EQ(NO_ERROR, ret);
        }
        pool->WriteUnlock();

    }
    pthread_exit(NULL);
}

TEST(Pool, MultiThreadStressTest)
{
    int const kNumThreads = 5;
    int const kNumTry = 50;
    size_t const kShelfSize = 8*1024*1024; // 8MB
    
    PoolId const pool_id = 1;
    Pool pool(pool_id);

    EXPECT_EQ(NO_ERROR, pool.Create(kShelfSize));
    EXPECT_EQ(NO_ERROR, pool.Open(true));

    pthread_t threads[kNumThreads];
    thread_argument args[kNumThreads];
    int ret=0;
    
    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Create worker " << i << std::endl;
        args[i].id = i;
        args[i].pool = &pool;
        args[i].count = kNumTry;
        ret = pthread_create(&threads[i], NULL, worker, (void*)&args[i]);
        ASSERT_EQ(0, ret);
    }

    void *status;
    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Join worker " << i << std::endl;
        ret = pthread_join(threads[i], &status);
        ASSERT_EQ(0, ret);
    }

    EXPECT_EQ(NO_ERROR, pool.Recover());    
    EXPECT_EQ(NO_ERROR, pool.Close(false));        
    EXPECT_EQ(NO_ERROR, pool.Destroy());
}


void RandomlyAddNewRemoveShelf(PoolId pool_id)
{
    Pool pool(pool_id);
    EXPECT_EQ(NO_ERROR, pool.Open(false));

    int count = 500;
    for (int i=0; i<count; i++)
    {
        ShelfIndex shelf_idx = (ShelfIndex)rand_uint32(0, pool.Size()-1);
        //ShelfIndex shelf_idx = 0;
        switch (rand_uint32(0,3))
        {
        case 0:
            // Recover
            pool.Recover();
            break;
        case 1:
            // AddShelf
            pool.AddShelf(shelf_idx);
            break;
        case 2:
            // RemoveShelf
            pool.RemoveShelf(shelf_idx);
            break;
        case 3:
            // NewShelf
            pool.NewShelf(shelf_idx);
            break;
        default:
            ASSERT_TRUE(false);
        }
    }
    EXPECT_EQ(NO_ERROR, pool.Close(false));
}

// multi-process (single thread per process) test
// slow
TEST(Pool, MultiProcessStressTest)
{
    PoolId const pool_id = 1;
    size_t const kShelfSize = 8*1024*1024; // 8MB
    Pool pool(pool_id);
    EXPECT_EQ(NO_ERROR, pool.Create(kShelfSize));

    int const process_count = 8;
    pid_t pid[process_count];

    for (int i=0; i< process_count; i++)
    {
        pid[i] = fork();
        ASSERT_LE(0, pid[i]);
        if (pid[i]==0)
        {
            // child
            RandomlyAddNewRemoveShelf(pool_id);
            exit(0); // this will leak memory (see valgrind output)
        }
        else
        {
            // parent
            continue;
        }
    }

    for (int i=0; i< process_count; i++)
    {    
        int status;
        waitpid(pid[i], &status, 0);
    }


    EXPECT_EQ(NO_ERROR, pool.Open(false));
    EXPECT_EQ(NO_ERROR, pool.Recover());
    EXPECT_EQ(NO_ERROR, pool.Close(false));    
    EXPECT_EQ(NO_ERROR, pool.Destroy());        
}
#endif

int main(int argc, char** argv)
{
    InitTest();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

