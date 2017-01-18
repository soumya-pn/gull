#include <fcntl.h> // for O_RDWR
#include <sys/mman.h> // for PROT_READ, PROT_WRITE, MAP_SHARED
#include <pthread.h>

#include <gtest/gtest.h>
#include "nvmm/nvmm_fam_atomic.h"

#include "nvmm/memory_manager.h"
#include "shelf_mgmt/pool.h"
#include "test_common/test.h"
#include "shelf_usage/freelists.h"

using namespace nvmm;
ShelfName shelf_name;

std::random_device r;
std::default_random_engine e1(r());
uint32_t rand_uint32(uint32_t min, uint32_t max)
{
    std::uniform_int_distribution<uint32_t> uniform_dist(min, max);
    return uniform_dist(e1);
}

// single-threaded
TEST(MemoryManager, Region)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    int64_t* address = NULL;

    MemoryManager *mm = MemoryManager::GetInstance();
    Region *region = NULL;
    
    // create a region
    EXPECT_EQ(ID_NOT_FOUND, mm->FindRegion(pool_id, &region));
    EXPECT_EQ(NO_ERROR, mm->CreateRegion(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateRegion(pool_id, size));

    // get the existing region
    EXPECT_EQ(NO_ERROR, mm->FindRegion(pool_id, &region));
    
    // write a value
    EXPECT_EQ(NO_ERROR, region->Open(O_RDWR));
    EXPECT_EQ(NO_ERROR, region->Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&address));
    fam_atomic_64_write(address, 123LL);
    // TODO: the buggy line....
    EXPECT_EQ(NO_ERROR, region->Unmap(address, size));
    EXPECT_EQ(NO_ERROR, region->Close());        

    delete region;


    //sleep(3);
    
    // get the existing region
    EXPECT_EQ(NO_ERROR, mm->FindRegion(pool_id, &region));

    // read it back
    EXPECT_EQ(NO_ERROR, region->Open(O_RDWR));
    EXPECT_EQ(NO_ERROR, region->Map(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&address));
    EXPECT_EQ(123LL, fam_atomic_64_read(address));
    EXPECT_EQ(NO_ERROR, region->Unmap(address, size));
    EXPECT_EQ(NO_ERROR, region->Close());        

    delete region;

    // destroy the region
    EXPECT_EQ(NO_ERROR, mm->DestroyRegion(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyRegion(pool_id));
}

TEST(MemoryManager, Heap)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;
    
    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    
    // write a value
    EXPECT_EQ(NO_ERROR, heap->Open());
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));

    // // read it back
    EXPECT_EQ(NO_ERROR, heap->Open());
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;

    // destroy the heap
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

// TODO
//#ifndef ZONE
TEST(MemoryManager, HeapWithMapUnmapPointer)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    GlobalPtr ptr[10];

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;
    
    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    
    EXPECT_EQ(NO_ERROR, heap->Open());
    for (int i=0; i<10; i++)
    {
        ptr[i] = heap->Alloc(sizeof(int));
        EXPECT_TRUE(ptr[i].IsValid());
        int *int_ptr = NULL;
        EXPECT_EQ(NO_ERROR,
                  mm->MapPointer(ptr[i], sizeof(int), NULL, PROT_READ|PROT_WRITE, MAP_SHARED,
                                            (void **)&int_ptr)
                  );
        *int_ptr = i;            
        EXPECT_EQ(NO_ERROR, mm->UnmapPointer(ptr[i],(void *)int_ptr, sizeof(int)));
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));    
    EXPECT_EQ(NO_ERROR, heap->Open());    
    for (int i=0; i<10; i++)
    {
        int *int_ptr = NULL;
        EXPECT_EQ(NO_ERROR,
                  mm->MapPointer(ptr[i], sizeof(int), NULL, PROT_READ|PROT_WRITE, MAP_SHARED,
                                            (void **)&int_ptr)
                  );
        EXPECT_EQ(i, *int_ptr);        
        EXPECT_EQ(NO_ERROR, mm->UnmapPointer(ptr[i], (void *)int_ptr, sizeof(int)));
        heap->Free(ptr[i]);
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // destroy a heap
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));    
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}
//#endif

TEST(MemoryManager, HeapWithGlobalLocalPtr)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    GlobalPtr ptr[10];

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;
    
    // create a heap
    EXPECT_EQ(ID_NOT_FOUND, mm->FindHeap(pool_id, &heap));
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    EXPECT_EQ(ID_FOUND, mm->CreateHeap(pool_id, size));

    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    
    EXPECT_EQ(NO_ERROR, heap->Open());
    for (int i=0; i<10; i++)
    {
        ptr[i] = heap->Alloc(sizeof(int));
        EXPECT_TRUE(ptr[i].IsValid());
        int *int_ptr = (int*)mm->GlobalToLocal(ptr[i]);
#ifndef ZONE
        EXPECT_EQ(ptr[i], mm->LocalToGlobal(int_ptr));
#endif        
        EXPECT_TRUE(int_ptr != NULL);
        *int_ptr = i;            
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));    
    EXPECT_EQ(NO_ERROR, heap->Open());    
    for (int i=0; i<10; i++)
    {
        int *int_ptr = (int*)mm->GlobalToLocal(ptr[i]);
#ifndef ZONE
        EXPECT_EQ(ptr[i], mm->LocalToGlobal(int_ptr));
#endif        
        EXPECT_TRUE(int_ptr != NULL);
        EXPECT_EQ(i, *int_ptr);        
        heap->Free(ptr[i]);
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // destroy a heap
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));    
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));
}

TEST(MemoryManager, HeapHugeObjects)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    size_t obj_size = size/128; // 1MB
    char *buf = new char[obj_size];
    assert(buf != NULL);
    GlobalPtr ptr[10];

    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;
    
    // create a heap
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(pool_id, size));
    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));
    
    EXPECT_EQ(NO_ERROR, heap->Open());
    for (int i=0; i<3; i++)
    {
        ptr[i] = heap->Alloc(obj_size);
        EXPECT_TRUE(ptr[i].IsValid());
        char *char_ptr = NULL;
        EXPECT_EQ(NO_ERROR,
                  mm->MapPointer(ptr[i], obj_size, NULL, PROT_READ|PROT_WRITE, MAP_SHARED,
                                            (void **)&char_ptr)
                  );        
        memset(buf, i, obj_size);
        memcpy(char_ptr, buf, obj_size);
        EXPECT_EQ(NO_ERROR, mm->UnmapPointer(ptr[i],(void *)char_ptr, sizeof(int)));
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // get the existing heap
    EXPECT_EQ(NO_ERROR, mm->FindHeap(pool_id, &heap));    
    EXPECT_EQ(NO_ERROR, heap->Open());    
    for (int i=0; i<3; i++)
    {
        char *char_ptr = NULL;
        EXPECT_EQ(NO_ERROR,
                  mm->MapPointer(ptr[i], obj_size, NULL, PROT_READ|PROT_WRITE, MAP_SHARED,
                                            (void **)&char_ptr)
                  );
        memset(buf, i, obj_size);
        EXPECT_EQ(0, memcmp(buf, char_ptr, obj_size));
        EXPECT_EQ(NO_ERROR, mm->UnmapPointer(ptr[i], (void *)char_ptr, sizeof(int)));
        heap->Free(ptr[i]);
    }
    EXPECT_EQ(NO_ERROR, heap->Close());        

    delete heap;
    
    // destroy a heap
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(pool_id));    
    EXPECT_EQ(ID_NOT_FOUND, mm->DestroyHeap(pool_id));

    delete buf;
}

#ifndef LFS
// multi-threaded
#ifndef ALPS
struct thread_argument{
    int  id;
    int count;
};

void *worker(void *thread_arg)
{
    ErrorCode ret = NO_ERROR;
    thread_argument *arg = (thread_argument*)thread_arg;
    int count = arg->count;
    assert(count != 0);    
    MemoryManager *mm = MemoryManager::GetInstance();
    size_t size = 8*1024*1024LLU; // 8MB
    for (int i=0; i<count; i++)
    {
        PoolId pool_id = (PoolId)rand_uint32(1, Pool::kMaxPoolCount-1); // 0 is reserved
        Region *region = NULL;
        Heap *heap = NULL;
        
        //ShelfIndex shelf_idx = 0;
        switch (rand_uint32(0,5))
        {
        case 0:
            mm->CreateRegion(pool_id, size);
            break;
        case 1:
            mm->DestroyRegion(pool_id);
            break;
        case 2:
            ret = mm->FindRegion(pool_id, &region);
            if (ret == NO_ERROR && region != NULL)
                delete region;
            break;
        case 3:
            mm->CreateHeap(pool_id, size);
            break;
        case 4:
            mm->DestroyHeap(pool_id);
            break;
        case 5:
            ret = mm->FindHeap(pool_id, &heap);
            if (ret == NO_ERROR && heap != NULL)
                delete heap;
            break;
        default:
            assert(0);
            break;
        }
    }
    pthread_exit(NULL);    
}

TEST(MemoryManager, MultiThreadStressTest)
{
    int const kNumThreads = 5;
    int const kNumTry = 10;
    
    pthread_t threads[kNumThreads];
    thread_argument args[kNumThreads];
    int ret=0;
    
    for(int i=0; i<kNumThreads; i++)
    {
        args[i].id = i;
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

    // delete all created pools    
    MemoryManager *mm = MemoryManager::GetInstance();
    for (PoolId pool_id = 1; pool_id < Pool::kMaxPoolCount; pool_id++)
    {
        // TODO: this is safe for now....
        (void)mm->DestroyHeap(pool_id);
        (void)mm->DestroyRegion(pool_id);
        Pool pool(pool_id);
        (void)pool.Destroy();
    }
}
#endif // ALPS
#endif // LFS

// multi-process
void LocalAllocRemoteFree(PoolId heap_pool_id, ShelfId comm_shelf_id)
{
    // open the comm
    std::string path = shelf_name.Path(comm_shelf_id);
    ShelfFile shelf(path);
    void *address = NULL;
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    size_t length = shelf.Size();    
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&address));
    FreeLists comm(address, length);
    EXPECT_EQ(NO_ERROR, comm.Open());    

    
    // =======================================================================    
    // get the existing heap
    MemoryManager *mm = MemoryManager::GetInstance();
    Heap *heap = NULL;
    EXPECT_EQ(NO_ERROR, mm->FindHeap(heap_pool_id, &heap));
    EXPECT_TRUE(heap != NULL);
    
    EXPECT_EQ(NO_ERROR, heap->Open());

    int count = 500;
    size_t alloc_unit = 128*1024; // 128KB
    GlobalPtr ptr;
    for (int i=0; i<count; i++)
    {
        if (comm.GetPointer(0, ptr) == NO_ERROR)
        {
            uint64_t *uint_ptr = (uint64_t*)mm->GlobalToLocal(ptr);
#ifndef ZONE            
            EXPECT_EQ(ptr, mm->LocalToGlobal(uint_ptr));
#endif            
            EXPECT_TRUE(uint_ptr != NULL);
            EXPECT_EQ(ptr.ToUINT64(), *uint_ptr);
            heap->Free(ptr);
        }

        ptr = heap->Alloc(alloc_unit);
        if (ptr.IsValid() == false)
        {
            std::cout << "Alloc failed" << std::endl;
        }
        else
        {
            uint64_t *uint_ptr = (uint64_t*)mm->GlobalToLocal(ptr);
#ifndef ZONE            
            EXPECT_EQ(ptr, mm->LocalToGlobal(uint_ptr));
#endif            
            EXPECT_TRUE(uint_ptr != NULL);
            *uint_ptr = ptr.ToUINT64();
            EXPECT_EQ(NO_ERROR, comm.PutPointer(0, ptr));
        }        
    }

    EXPECT_EQ(NO_ERROR, heap->Close());
    delete heap;
    // =======================================================================
    
    // close the comm
    EXPECT_EQ(NO_ERROR, comm.Close());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(address, length));
    EXPECT_EQ(NO_ERROR, shelf.Close());            
}

TEST(MemoryManager, MultiProcessHeap)
{
    int const process_count = 16;

    // create a shelf for communication among processes
    // use the FreeLists
    // TODO: make it a shelf_usage class?    
    ShelfId const comm_shelf_id(15,15);
    std::string path = shelf_name.Path(comm_shelf_id);
    ShelfFile shelf(path);
    size_t length = 128*1024*1024LLU; 
    size_t list_count = 1;
    void* address = NULL;    
    EXPECT_EQ(NO_ERROR, shelf.Create(S_IRUSR|S_IWUSR, length));
    EXPECT_EQ(NO_ERROR, shelf.Open(O_RDWR));
    EXPECT_EQ(NO_ERROR, shelf.Map(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, 0, (void**)&address));

    FreeLists comm(address, length);
    EXPECT_EQ(NO_ERROR, comm.Create(list_count));

    // =======================================================================
    // create the DistHeap
    MemoryManager *mm = MemoryManager::GetInstance();
    PoolId heap_pool_id = 1; // the pool id of the DistHeap
    size_t size = 128*1024*1024LLU;   
    EXPECT_EQ(NO_ERROR, mm->CreateHeap(heap_pool_id, size));

    pid_t pid[process_count];

    for (int i=0; i< process_count; i++)
    {
        pid[i] = fork();
        ASSERT_LE(0, pid[i]);
        if (pid[i]==0)
        {
            //nvmm::GlobalEpoch::GetInstance().resetEpochSystemInChildAfterFork();            
            // child
            LocalAllocRemoteFree(heap_pool_id, comm_shelf_id);
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
    
    // destroy the heap
    EXPECT_EQ(NO_ERROR, mm->DestroyHeap(heap_pool_id));
    // =======================================================================

    // destroy the shelf for communication
    EXPECT_EQ(NO_ERROR, comm.Destroy());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(address, length));
    EXPECT_EQ(NO_ERROR, shelf.Close());        
    EXPECT_EQ(NO_ERROR, shelf.Destroy());        
}

void stress(int count)
{
    assert(count != 0);    
    ErrorCode ret = NO_ERROR;
    MemoryManager *mm = MemoryManager::GetInstance();
    size_t size = 8*1024*1024LLU; // 8MB
    for (int i=0; i<count; i++)
    {
        PoolId pool_id = (PoolId)rand_uint32(1, Pool::kMaxPoolCount-1); // 0 is reserved
        Region *region = NULL;
        Heap *heap = NULL;
        
        //ShelfIndex shelf_idx = 0;
        switch (rand_uint32(0,5))
        {
        case 0:
            mm->CreateRegion(pool_id, size);
            break;
        case 1:
            //mm->DestroyRegion(pool_id);
            break;
        case 2:
            ret = mm->FindRegion(pool_id, &region);
            if (ret == NO_ERROR && region != NULL)
                delete region;
            break;
        case 3:
            mm->CreateHeap(pool_id, size);
            break;
        case 4:
            //mm->DestroyHeap(pool_id);
            break;
        case 5:
            ret = mm->FindHeap(pool_id, &heap);
            if (ret == NO_ERROR && heap != NULL)
                delete heap;
            break;
        default:
            assert(0);
            break;
        }
    }
}

// TODO: there seems to be a bug in libfam-atomic that may cause this test case to fail
#ifdef FAM1
TEST(MemoryManager, MultiProcessNVMM)
{
    int const process_count = 4;
    int const loop_count = 100;
    
    pid_t pid[process_count];
    for (int i=0; i< process_count; i++)
    {
        pid[i] = fork();
        ASSERT_LE(0, pid[i]);
        if (pid[i]==0)
        {
            //nvmm::GlobalEpoch::GetInstance().resetEpochSystemInChildAfterFork();            
            // child
            stress(loop_count);
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

    // delete all created pools    
    MemoryManager *mm = MemoryManager::GetInstance();
    for (PoolId pool_id = 1; pool_id < Pool::kMaxPoolCount; pool_id++)
    {
        // TODO: this is safe for now....
        (void)mm->DestroyHeap(pool_id);
        (void)mm->DestroyRegion(pool_id);
        Pool pool(pool_id);
        (void)pool.Destroy();
    }    
}
#endif

int main(int argc, char** argv)
{
    InitTest(nvmm::fatal, true);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


