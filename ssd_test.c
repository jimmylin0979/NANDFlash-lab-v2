/*
  FUSE ssdlient: FUSE ioctl example client
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include "ssd_fuse_header.h"
#include <time.h>

const char *usage =
    "Usage: ssd_fuse SSD_FILE COMMAND\n"
    "\n"
    "COMMANDS\n"
    "  l    : get logic size \n"
    "  p    : get physical size \n"
    "  r SIZE [OFF] : read SIZE bytes @ OFF (dfl 0) and output to stdout\n"
    "  w SIZE [OFF] : write SIZE bytes @ OFF (dfl 0) from random\n"
    "  W    : write amplification factor\n"
    "\n";

char *simulated_nand;
char *tmp_buf;

static int do_rw(FILE *fd, int is_read, size_t size, off_t offset)
{
    char *buf;
    int idx;
    ssize_t ret;
    buf = calloc(1, size);

    if (!buf)
    {
        fprintf(stderr, "failed to allocated %zu bytes\n", size);
        return -1;
    }
    if (is_read)
    {
        printf("dut do read size %ld, off %d\n", size, (int)offset);
        fseek(fd, offset, SEEK_SET);
        ret = fread(buf, 1, size, fd);
        printf("[COMM] success, return with size %ld\n", sizeof(buf) / sizeof(char));
        if (ret >= 0)
        {
            tmp_buf = malloc(size * sizeof(char));
            memcpy(tmp_buf, buf, size);
            // fwrite(buf, 1, ret, stdout);
            // printf("\n");
        }
    }
    else
    {
        for (idx = 0; idx < size; idx++)
        {
            buf[idx] = (idx % 78) + 48;
        }
        printf("dut do write size %ld, off %d\n", size, (int)offset);

        // write the data to the simulated NAND too
        for (idx = 0; idx < size; idx++)
        {
            simulated_nand[offset + idx] = buf[idx];
        }

        fseek(fd, offset, SEEK_SET);
        printf("fseek \n");
        ret = fwrite(buf, 1, size, fd);
        // arg.size = fread(arg.buf, 1, size, stdin);
        fprintf(stderr, "Writing %zu bytes\n", size);
    }
    if (ret < 0)
    {
        perror("ioctl");
    }

    free(buf);
    return ret;
}

int ssd_command(char *path, char cmd, size_t offset, size_t size)
{
    size_t param[2] = {};
    FILE *fptr;
    int fd, rc;

    switch (cmd)
    {
    // case 'l':
    //     fd = open(path, O_RDWR);
    //     if (fd < 0)
    //     {
    //         perror("open");
    //         return 1;
    //     }
    //     if (ioctl(fd, SSD_GET_LOGIC_SIZE, &size))
    //     {
    //         perror("ioctl");
    //         goto error;
    //     }
    //     printf("%zu\n", size);
    //     close(fd);
    //     return 0;
    // case 'p':
    //     fd = open(path, O_RDWR);
    //     if (fd < 0)
    //     {
    //         perror("open");
    //         return 1;
    //     }
    //     if (ioctl(fd, SSD_GET_PHYSIC_SIZE, &size))
    //     {
    //         perror("ioctl");
    //         goto error;
    //     }
    //     printf("%zu\n", size);
    //     close(fd);
    //     return 0;
    case 'r':
    case 'w':
        if (!(fptr = fopen(path, "r+")))
        {
            perror("open");
            return 1;
        }

        param[0] = size;
        param[1] = offset;
        rc = do_rw(fptr, cmd == 'r', param[0], param[1]);
        if (rc < 0)
        {
            goto error;
        }
        printf("\ntransferred %d bytes \n", rc);

        fclose(fptr);
        return 0;
    // case 'W':
    //     fd = open(path, O_RDWR);
    //     if (fd < 0)
    //     {
    //         perror("open");
    //         return 1;
    //     }
    //     double wa;
    //     if (ioctl(fd, SSD_GET_WA, &wa))
    //     {
    //         perror("ioctl");
    //         goto error;
    //     }
    //     printf("%f\n", wa);
    //     close(fd);
    //     return 0;
    case 'e':
        fd = open(path, O_RDWR);
        unsigned long long eraseFrame = 0;

        param[0] = offset;
        param[1] = size;
        for (int i = 0; i < 2; i++)
        {
            int tmp = param[i];
            eraseFrame = eraseFrame << 32;
            eraseFrame |= tmp;
        }
        if (fd < 0)
        {
            perror("open");
            return 1;
        }
        if (ioctl(fd, SSD_LOGIC_ERASE, &eraseFrame))
        {
            perror("ioctl");
            goto error;
        }

        int idx = 0;
        for (idx = 0; idx < size; idx++)
        {
            simulated_nand[offset + idx] = 0;
        }
        printf("erase start %lu, erase size %lu\n", param[0], param[1]);
        close(fd);
        return 0;
    }
error:
    return 1;
}

int main(int argc, char **argv)
{
    // initial the while nand block
    simulated_nand = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 * sizeof(char));
    memset(simulated_nand, 0, sizeof(char) * LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024);

    // read log
    FILE *fptr = fopen("test.log", "r");
    int testCase_idx = 0, idx = 0;
    int c;
    if (fptr != NULL)
    {
        while ((c = fgetc(fptr)) != EOF)
        {
            simulated_nand[idx++] = (char)c;
            printf("%c", c);
        }
        printf("read %d chars from file\n", idx);
        fclose(fptr);
    }

    // random seed
    time_t t;
    srand((unsigned)time(&t));

    //
    char *path;
    if (argc < 2)
    {
        printf("usage: %s <path>\n", argv[0]);
        exit(1);
    }
    path = argv[1];

    char cmd;
    int NUM_TESTCASE = 10000;
    for (testCase_idx = 0; testCase_idx < NUM_TESTCASE; testCase_idx++)
    {
        // random ssd w, e
        int TEST_MAX_LENGTH = 1024;
        size_t offset = rand() % TEST_MAX_LENGTH; // (LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024);
        size_t size = rand() % 1024 + 1;          // (LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024) + 1;

        // int test_choice = rand() % 4;
        // switch (test_choice)
        // {
        // case 0:
        //     // offset 512k, size 512k
        //     offset = (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        //     size = (size / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        //     size = (size == 0) ? PHYSICAL_DATA_SIZE_BYTES_PER_PAGE : size;
        //     break;
        // case 1:
        //     // offset 512k + x, size 512k
        //     size = (size / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        //     size = (size == 0) ? PHYSICAL_DATA_SIZE_BYTES_PER_PAGE : size;
        //     break;
        // case 2:
        //     // offset 512k, size 512k + x
        //     offset = (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        //     break;
        // case 3:
        //     // offset 512k + x, size 512k + x
        //     break;
        // }

        if (offset + size > TEST_MAX_LENGTH)
        {
            size = TEST_MAX_LENGTH - offset - 1;
        }

        // TODO
        int cmd_choice = rand() % 3;
        switch (cmd_choice)
        {
        case 0:
            cmd = 'r';
            break;
        case 1:
            cmd = 'w';
            break;
        case 2:
            cmd = 'e';
            break;
        default:
            break;
        }

        printf("===============================================================\n");
        printf("[COMM] %02dth, ssd %c with offset %lu and size %lu\n", testCase_idx, cmd, offset, size);
        ssd_command(path, cmd, offset, size);

        // compare the result
        if (cmd == 'r')
        {
            int res = 0;
            for (idx = 0; idx < size; idx++)
            {
                if (simulated_nand[offset + idx] != tmp_buf[idx])
                {
                    res = 1;
                    break;
                }
            }

            printf("Ground truth:\n");
            for (idx = 0; idx < size; idx++)
                printf("%c", simulated_nand[offset + idx]);
            printf("\n");

            //
            if (res == 1)
            {
                printf("\033[0;31m");
                printf("[FAIL] %02dth, ssd %c with offset %lu and size %lu\n", testCase_idx, cmd, offset, size);
                printf("\033[0m\n");
                printf("Ours:\n");
                for (idx = 0; idx < size; idx++)
                    printf("%c", tmp_buf[idx]);
                printf("\n");
                break;
            }
            else
            {
                printf("\033[0;32m");
                printf("[PASS] %02dth, ssd %c with offset %lu and size %lu\n", testCase_idx, cmd, offset, size);
                printf("\033[0m\n");
            }
        }

        //
        free(tmp_buf);
        tmp_buf = NULL;

        // write test log
        FILE *fptr = fopen("test.log", "w");
        for (idx = 0; idx < LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024; idx++)
        {
            fprintf(fptr, "%c", simulated_nand[idx]);
        }
        fclose(fptr);
    }
}