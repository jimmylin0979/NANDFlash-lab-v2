/*
  FUSE-ioctl: ioctl support for FUSE
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#define PHYSICAL_NAND_NUM (50)
#define LOGICAL_NAND_NUM (40)
#define PHYSICAL_DATA_SIZE_BYTES_PER_PAGE (512)
#define PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE (8)

#define NAND_SIZE_KB (10)
#define SLC_NAND_SIZE_KB (NAND_SIZE_KB / 2)

#define INVALID_PCA (0xFFFFFFFF)
#define FULL_PCA (0xFFFFFFFE)

#define SLC_mode (0x00)          // 0000 0000
#define MLC_mode (0x80)          // 1000 0000
#define WRITE_BUFFER_SIZE_KB (5) // 5K = 512B * 10
#define CACHE_BUFFER_SIZE (10)        // 

#define PAGE_NUMBER_PER_NAND (NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
#define SLC_PAGE_NUMBER_PER_NAND (SLC_NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
#define WRITE_BUFFER_PAGE_NUM (WRITE_BUFFER_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)

#define NAND_LOCATION "/home/jimmylin0979/Desktop/NANDFlash-lab/NANDs"
#define LOG_LOCATION "/home/jimmylin0979/Desktop/NANDFlash-lab/log"
// #define OUTPUT_LOCATION "/home/jimmylin0979/Desktop/NANDFlash-lab/testCase/output"
#define TEST_LOG_LOCATION "/home/jimmylin0979/Desktop/NANDFlash-lab/test.log"
#define TEST_CACHE_LOG_LOCATION "/home/jimmylin0979/Desktop/NANDFlash-lab/test_cache.log"

enum
{
  SSD_GET_LOGIC_SIZE  = _IOR('E', 0, size_t),
  SSD_GET_PHYSIC_SIZE = _IOR('E', 1, size_t),
  SSD_GET_WA          = _IOR('E', 2, size_t),
  SSD_LOGIC_ERASE     = _IOW('E', 3, size_t),
  SSD_FLUSH           = _IO('E', 4),
  SSD_SHOW_L2P        = _IO('E', 5),
  SSD_STORE_WOCACHE   = _IO('E', 6),
};
