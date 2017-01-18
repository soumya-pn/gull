#include <pthread.h>
#include <gtest/gtest.h>

#include "nvmm/memory_manager.h"
#include "allocator/dist_heap.h"
#include "shelf_usage/freelists.h"
#include "shelf_mgmt/shelf_file.h"
#include "shelf_mgmt/shelf_name.h"

#include "test_common/test.h"

using namespace nvmm;

// single-threaded
TEST(DistHeap, CreateDestroyExist)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    DistHeap heap(pool_id);

    // create a heap
    EXPECT_EQ(NO_ERROR, heap.Create(size));
    EXPECT_TRUE(heap.Exist());
    EXPECT_EQ(POOL_FOUND, heap.Create(size));

    // Destroy a heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());
    EXPECT_FALSE(heap.Exist());
    EXPECT_EQ(POOL_NOT_FOUND, heap.Destroy());
}

TEST(DistHeap, OpenCloseSize)
{
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB

    // open a heap that does not exist
    DistHeap heap(pool_id);
    EXPECT_EQ(HEAP_OPEN_FAILED, heap.Open());
    
    // create a heap
    EXPECT_EQ(NO_ERROR, heap.Create(size));

    // open the heap
    EXPECT_EQ(NO_ERROR, heap.Open());

    // close the heap
    EXPECT_EQ(NO_ERROR, heap.Close());
    
    // Destroy a heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());
}

TEST(DistHeap, AllocFreeAccess)
{
    PoolId pool_id = 1;
    GlobalPtr ptr[10];
    size_t size = 128*1024*1024LLU; // 128 MB
    DistHeap heap(pool_id);

    // create a heap
    EXPECT_EQ(NO_ERROR, heap.Create(size));

    EXPECT_EQ(NO_ERROR, heap.Open());
    for (int i=0; i<10; i++)
    {
        ptr[i] = heap.Alloc(sizeof(int));
        EXPECT_TRUE(ptr[i].IsValid());
        int *int_ptr = (int*)heap.GlobalToLocal(ptr[i]);
        *int_ptr = i;            
        
    }
    EXPECT_EQ(NO_ERROR, heap.Close());        

    EXPECT_EQ(NO_ERROR, heap.Open());    
    for (int i=0; i<10; i++)
    {
        int *int_ptr = (int*)heap.GlobalToLocal(ptr[i]);
        EXPECT_EQ(i, *int_ptr);        
        heap.Free(ptr[i]);
    }
    EXPECT_EQ(NO_ERROR, heap.Close());        

    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());
}

// multi-threaded
struct thread_argument{
    int  id;
    DistHeap *heap;
    int try_count;
    GlobalPtr *ptr;
};

void *writer(void *thread_arg)
{
    thread_argument *arg = (thread_argument*)thread_arg;

    int count = arg->try_count;
    assert(count != 0);
    arg->ptr = new GlobalPtr[count];
    assert(arg->ptr != NULL);
    GlobalPtr *ptr = arg->ptr;
    
    for (int i=0; i<count; i++)
    {
        ptr[i] = arg->heap->Alloc(sizeof(int));
        if (ptr[i].IsValid() == false)
        {
            std::cout << "Thread " << arg->id << ": alloc failed" << std::endl;
        }
        else
        {
            int *int_ptr = (int*)arg->heap->GlobalToLocal(ptr[i]);
            *int_ptr = i;            
        }
    }

    pthread_exit(NULL);
}

void *reader(void *thread_arg)
{
    thread_argument *arg = (thread_argument*)thread_arg;

    int count = arg->try_count;
    assert(count != 0);

    assert(arg->ptr != NULL);    
    GlobalPtr *ptr = arg->ptr;
    
    for (int i=0; i<count; i++)
    {
        if (ptr[i].IsValid() == true)
        {
            int *int_ptr = (int*)arg->heap->GlobalToLocal(ptr[i]);
            EXPECT_EQ(i, *int_ptr);        
            arg->heap->Free(ptr[i]);
        }
    }

    delete [] ptr;
    
    pthread_exit(NULL);
}

TEST(DistHeap, MultiThread)
{
    int const kNumThreads = 10;
    int const kNumTry = 100;

    PoolId pool_id=1;
    size_t size = 128*1024*1024LLU; // 128 MB
    DistHeap heap(pool_id);

    // create a heap
    EXPECT_EQ(NO_ERROR, heap.Create(size));
    
    EXPECT_EQ(NO_ERROR, heap.Open());
    pthread_t threads[kNumThreads];
    thread_argument args[kNumThreads];
    int ret=0;
    void *status;
    
    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Create writer " << i << std::endl;
        args[i].id = i;
        args[i].heap = &heap;
        args[i].try_count = kNumTry;
        ret = pthread_create(&threads[i], NULL, writer, (void*)&args[i]);
        assert (0 == ret);
    }

    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Join writer " << i << std::endl;
        ret = pthread_join(threads[i], &status);
        assert (0 == ret);
    }

    EXPECT_EQ(NO_ERROR, heap.Close());        


    EXPECT_EQ(NO_ERROR, heap.Open());
    
    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Create reader " << i << std::endl;
        args[i].id = i;
        args[i].heap = &heap;
        args[i].try_count = kNumTry;
        ret = pthread_create(&threads[i], NULL, reader, (void*)&args[i]);
        assert (0 == ret);
    }

    for(int i=0; i<kNumThreads; i++)
    {
        //std::cout << "Join reader " << i << std::endl;
        ret = pthread_join(threads[i], &status);
        assert (0 == ret);
    }
    
    EXPECT_EQ(NO_ERROR, heap.Close());        
    // destroy the heap
    EXPECT_EQ(NO_ERROR, heap.Destroy());
}

// multi-process
void LocalAllocLocalFree(PoolId pool_id)
{
    DistHeap heap(pool_id);
    EXPECT_EQ(NO_ERROR, heap.Open());

    int count = 500;
    size_t alloc_unit = 16*1024; // 16KB
    GlobalPtr *ptr = new GlobalPtr[count];
    assert(ptr != NULL);
    for (int i=0; i<count; i++)
    {
        ptr[i] = heap.Alloc(alloc_unit);
        if (ptr[i].IsValid() == false)
        {
            std::cout << "LocalAllocLocalFree: Alloc failed" << std::endl;
        }
        else
        {
            int *int_ptr = (int*)heap.GlobalToLocal(ptr[i]);
            *int_ptr = i;
        }
    }

    for (int i=0; i<count; i++)
    {
        if (ptr[i].IsValid() == true)
        {
            int *int_ptr = (int*)heap.GlobalToLocal(ptr[i]);
            EXPECT_EQ(i, *int_ptr);        
            heap.Free(ptr[i]);
        }
        else
        {
            std::cout << "Invalid pointer?" << std::endl;
        }
    }
    delete [] ptr;

    EXPECT_EQ(NO_ERROR, heap.Close());
}

TEST(DistHeap, LocalAllocLccalFree)
{
    int const process_count = 8;
    PoolId pool_id = 1;
    size_t size = 128*1024*1024LLU; // 128 MB
    DistHeap heap(pool_id);
    // create a heap
    EXPECT_EQ(NO_ERROR, heap.Create(size));
    
    pid_t pid[process_count];

    for (int i=0; i< process_count; i++)
    {
        pid[i] = fork();
        ASSERT_LE(0, pid[i]);
        if (pid[i]==0)
        {
            //          nvmm::GlobalEpoch::GetInstance().resetEpochSystemInChildAfterFork();
            // child
            LocalAllocLocalFree(pool_id);
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
    EXPECT_EQ(NO_ERROR, heap.Destroy());    
}

ShelfName shelf_name;

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
    DistHeap heap(heap_pool_id);
    EXPECT_EQ(NO_ERROR, heap.Open());

    int count = 500; // one shelf heap is sufficient
    //int count = 2000; // require more than one shelf heaps
    size_t alloc_unit = 16*1024; // 16KB
    GlobalPtr ptr;
    for (int i=0; i<count; i++)
    {
        if (comm.GetPointer(0, ptr) == NO_ERROR)
        {
            heap.Free(ptr);
        }

        ptr = heap.Alloc(alloc_unit);
        if (ptr.IsValid() == false)
        {
            std::cout << "LocalAllocRemoteFree: Alloc failed" << std::endl;
        }
        else
        {
            EXPECT_EQ(NO_ERROR, comm.PutPointer(0, ptr));
        }        
    }

    EXPECT_EQ(NO_ERROR, heap.Close());
    // =======================================================================

    
    // close the comm
    EXPECT_EQ(NO_ERROR, comm.Close());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(address, length));
    EXPECT_EQ(NO_ERROR, shelf.Close());            
}


TEST(DistHeap, LocalAllocRemoteFree)
{
    int const process_count = 8;

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
    PoolId heap_pool_id = 1; // the pool id of the DistHeap
    size_t size = 128*1024*1024LLU; // 128MB
    DistHeap heap(heap_pool_id);
    EXPECT_EQ(NO_ERROR, heap.Create(size));
    
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
    EXPECT_EQ(NO_ERROR, heap.Destroy());
    // =======================================================================

    // destroy the shelf for communication
    EXPECT_EQ(NO_ERROR, comm.Destroy());
    EXPECT_EQ(NO_ERROR, shelf.Unmap(address, length));
    EXPECT_EQ(NO_ERROR, shelf.Close());        
    EXPECT_EQ(NO_ERROR, shelf.Destroy());        
}

int main(int argc, char** argv)
{
    InitTest(nvmm::info, false);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


