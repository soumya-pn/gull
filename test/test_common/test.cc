#include <cstdlib> // for system()
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <boost/filesystem/operations.hpp>

#include "nvmm/nvmm_fam_atomic.h"
#include "nvmm/log.h"
#include "nvmm/error_code.h"
#include "nvmm/shelf_id.h"
#include "nvmm/root_shelf.h"

#include "test_common/test.h"


namespace nvmm {

// TODO: move this to nvmm level
// NOTE: this function must be run once and only once for every test
void InitTest(SeverityLevel level, bool to_console)
{
    // init boost::log
    if (to_console == true)
    {
        nvmm::init_log(level, "");
    }
    else
    {
        nvmm::init_log(level, "mm.log");
    }

    // remove previous files in SHELF_BASE_DIR
    std::string cmd = std::string("exec rm -f ") + SHELF_BASE_DIR + "/" + SHELF_USER + "* > nul";
    system(cmd.c_str());
    

#ifdef LFS        
    // check if SHELF_BASE_DIR exists
    boost::filesystem::path shelf_base_path = boost::filesystem::path(SHELF_BASE_DIR);
    if (boost::filesystem::exists(shelf_base_path) == false)
    {
        LOG(fatal) << "InitTest: LFS does not exist " << SHELF_BASE_DIR;
        exit(1);
    }

    // create a root shelf (for MemoryManager) if it does not exist
    std::string root_shelf_file = std::string(SHELF_BASE_DIR) + "/" + SHELF_USER + "_NVMM_ROOT";
    RootShelf root_shelf(root_shelf_file);
    if(root_shelf.Exist() == false)
    {
        if(root_shelf.Create()!=NO_ERROR)
        {
            LOG(fatal) << "InitTest: Failed to create the root shelf file " << root_shelf_file;
            exit(1);
        }
    }
#endif
}

} // namespace nvmm

