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
#define INVALID_PCA     (0xFFFFFFFF)
#define FULL_PCA     (0xFFFFFFFE)
#define NAND_LOCATION  "/home/brian/lab_fuse/ssd"

enum
{
    SSD_GET_LOGIC_SIZE   = _IOR('E', 0, size_t),
    SSD_GET_PHYSIC_SIZE   = _IOR('E', 1, size_t),
    SSD_GET_WA            = _IOR('E', 2, size_t),
    SSD_LOGIC_ERASE       = _IOW('E', 3, size_t),
};
