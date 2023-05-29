/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME "ssd_file"

#define DEBUG_WITH_SSD 1
#ifdef DEBUG_WITH_SSD
#define DEBUG_PRINT(x) printf x
#else
#define DEBUG_PRINT(x) \
    do                 \
    {                  \
    } while (0)
#endif

#define LBA_NUM (LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
#define LBA_NUM_PER_BLOCK (NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)

// -------------------------------------------------------------- //
// FTL related data structure

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block : 16;
    } fields;
};

PCA_RULE curr_pca;

/*** Tables ***/
unsigned int *L2P; // logical to physical table
unsigned int *IVC; // invalid count of a block
unsigned int *ERC; // erase count of a block
int *erasedSlot;

int *SLC; // record whether a block is in SLC mode or not

// int write_buffer_index = 0;
// char write_buffer[PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * WRITE_BUFFER_PAGE_NUM];

// A linked list that points blocks together
typedef struct node Node;
struct node
{
    int block;
    struct node *next;
};

typedef struct cacheEntry CacheEntry;
struct cacheEntry
{
    size_t lba;                                   // logical block addressing
    char data[PHYSICAL_DATA_SIZE_BYTES_PER_PAGE]; // data for this LBA
    struct cacheEntry *next;                      // pointer to the next cache entry in the list
    int erasedSlot;
};

/*** Linked List ***/
Node *ll_head_unusedBlock = NULL;
// Node *ll_rear_cleanBlock = NULL;

Node *ll_head_blockWriteOrder = NULL; // the order in which block is written

CacheEntry *ll_head_cache = NULL;

/*** Flags ***/
int flag_updateLog = 0;

// -------------------------------------------------------------- //
// temp
// an array to help re-construct the linked list of clean blocks
// unsigned int *unusedBlock;

// -------------------------------------------------------------- //

static int
ssd_resize(size_t new_size)
{
    // set logic size to new_size
    if (new_size > LOGICAL_NAND_NUM * PAGE_NUMBER_PER_NAND * (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE))
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        flag_updateLog = 1;
        return 0;
    }
}

static int ssd_expand(size_t new_size)
{
    // logic must less logic limit
    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

Node *delete_node_from_linkedList(Node *head, int block)
{
    Node *curr_block = head;
    Node *prev_block = NULL;
    while (curr_block != NULL)
    {
        if (curr_block->block == block)
        {
            // free the previous node if find the block is already exist in blockWriteOrder
            DEBUG_PRINT(("[DEBUG] delete_node_from_linkedList, successfully delete block = %d\n", block));
            if (prev_block != NULL)
            {
                prev_block->next = curr_block->next;
                free(curr_block);
            }
            else
            {
                head = head->next;
                free(curr_block);
            }
            break;
        }

        // forward to next node
        prev_block = curr_block;
        curr_block = curr_block->next;
    }

    return head;
}

Node *append_node_to_linkedList(Node *head, int block)
{
    //
    Node *new_block = (Node *)malloc(sizeof(Node));
    new_block->block = block;
    new_block->next = NULL;

    //
    if (!head)
    {
        head = new_block;
        return head;
    }

    //
    Node *prev_block = head;
    while (prev_block->next != NULL)
    {
        // forward to next node
        prev_block = prev_block->next;
    }
    prev_block->next = new_block;
    return head;
}

void print_linkedList(Node *head)
{
    //
    Node *curr_node = head;
    DEBUG_PRINT(("linkedList : "));
    while (curr_node != NULL)
    {
        DEBUG_PRINT(("%d -> ", curr_node->block));
        // forward to next node
        curr_node = curr_node->next;
    }
    DEBUG_PRINT(("\n"));
}

int get_num_of_linkedList(Node *head)
{
    //
    Node *curr_block = head;
    int num = 0;
    while (curr_block != NULL)
    {
        // forward to next node
        curr_block = curr_block->next;
        num++;
    }
    return num;
}

CacheEntry *append_node_to_cache(CacheEntry *head, CacheEntry *new_node)
{
    //
    if (!head)
    {
        head = new_node;
        return head;
    }

    //
    CacheEntry *prev_block = head;
    while (prev_block->next != NULL)
    {
        // forward to next node
        prev_block = prev_block->next;
    }
    prev_block->next = new_node;
    return head;
}

void print_cache(CacheEntry *head)
{
    //
    CacheEntry *curr_node = head;
    DEBUG_PRINT(("linkedList : "));
    while (curr_node != NULL)
    {
        DEBUG_PRINT(("%u -> ", curr_node->lba));
        // forward to next node
        curr_node = curr_node->next;
    }
    DEBUG_PRINT(("\n"));
}

int get_num_of_cache(CacheEntry *head)
{
    //
    CacheEntry *curr_block = head;
    int num = 0;
    while (curr_block != NULL)
    {
        // forward to next node
        curr_block = curr_block->next;
        num++;
    }
    return num;
}

void nand_write_log(char *logBuf, int size)
{
    if (size > 512)
        return;
    FILE *fileResult;
    size_t numWritten;
    fileResult = fopen(LOG_LOCATION, "w"); // Use absoluate path to avoid permission denied
    if (fileResult == NULL)
    {
        printf("fopen() failed with following error, %s\n", strerror(errno));
    }
    numWritten = fwrite(logBuf, sizeof(char), size, fileResult);
    printf("write %zu bytes\n", numWritten);
    fclose(fileResult);
    nand_write_size += 512;
}

void ftl_write_log()
{
    // TODO only write log when the update flag is toggled
    // if (flag_updateLog == 0)
    // {
    //     DEBUG_PRINT(("[DEBUG] ftl_write_log end\n"));
    //
    //     return;
    // }
    DEBUG_PRINT(("[DEBUG] ftl_write_log start\n"));

    int MAX_LOG_SIZE = 512;
    unsigned char *tmp_buf = malloc(MAX_LOG_SIZE * sizeof(unsigned char));
    memset(tmp_buf, 255, sizeof(unsigned char) * MAX_LOG_SIZE);
    int log_size = 0;

    /*** Store the block order into log ***/
    Node *curr_node = ll_head_blockWriteOrder;
    int idx = 0, i = 0;
    while (curr_node)
    {
        char c = curr_node->block;
        // BUG Even though curr_node->block is 1, the program still write 0 into log
        // Use snprintf carefully, especially care for the size_t n
        // snprintf(tmp_buf + log_size, 1, "%c", c % 256);
        tmp_buf[log_size] = c % 256;
        curr_node = curr_node->next;
        log_size += 1;
    }
    log_size = 50;
    DEBUG_PRINT(("[DEBUG] ftl_write_log, stage 1 block order complete, log_size %d\n", log_size));

    /*** Store the ERC into log ***/
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        sprintf(tmp_buf + log_size, "%c", ERC[idx]);
        log_size += 1;
    }
    DEBUG_PRINT(("[DEBUG] ftl_write_log, stage 2 ERC table complete, log_size %d\n", log_size));

    /*** store the WAF related data structures into log ***/
    // unsigned int is 2 ~ 4 bytes in c, suppose it is 2 bytes here
    sprintf(tmp_buf + log_size, "%c", physic_size / 65536);
    sprintf(tmp_buf + log_size + 1, "%c", physic_size / 256);
    sprintf(tmp_buf + log_size + 2, "%c", physic_size % 256);
    log_size += 3;
    sprintf(tmp_buf + log_size, "%c", logic_size / 65536);
    sprintf(tmp_buf + log_size + 1, "%c", logic_size / 256);
    sprintf(tmp_buf + log_size + 2, "%c", logic_size % 256);
    log_size += 3;
    sprintf(tmp_buf + log_size, "%c", host_write_size / 65536);
    sprintf(tmp_buf + log_size + 1, "%c", host_write_size / 256);
    sprintf(tmp_buf + log_size + 2, "%c", host_write_size % 256);
    log_size += 3;
    sprintf(tmp_buf + log_size, "%c", nand_write_size / 65536);
    sprintf(tmp_buf + log_size + 1, "%c", nand_write_size / 256);
    sprintf(tmp_buf + log_size + 2, "%c", nand_write_size % 256);
    log_size += 3;
    DEBUG_PRINT(("[DEBUG] ftl_write_log, stage 3  WAF data structures complete, log_size %d\n", log_size));

    /*** Store the erasedSlot into the log ***/
    int value = 0;
    for (idx = 0; idx < LBA_NUM; idx += 8)
    {
        value = 0;
        for (i = 0; i < 8; i++)
            value = value * 2 + erasedSlot[idx + i];
        sprintf(tmp_buf + log_size, "%c", value % 256);
        log_size += 1;
    }

    /*** store the number of SLC blocks ***/
    int num_slcBlock = 0;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        if (SLC[idx] != 0)
            num_slcBlock++;
    }
    sprintf(tmp_buf + log_size + 1, "%c", num_slcBlock % 256);
    log_size += 1;

    /*** Write log ***/
    DEBUG_PRINT(("[DEBUG] ftl_write_log, stage 4  call nand_write_log\n"));
    nand_write_log(tmp_buf, MAX_LOG_SIZE);
    free(tmp_buf);

    // reset flags
    flag_updateLog = 0;

    DEBUG_PRINT(("[DEBUG] ftl_write_log end\n"));
}

size_t spare_read(unsigned int pca);
int ftl_restore(unsigned int *unusedBlock)
{

    DEBUG_PRINT(("[DEBUG] ftl_restore start\n"));

    // call nand_read_log() to read the log
    unsigned char *log_buf = calloc(512, sizeof(unsigned char));
    size_t log_size = nand_read_log(log_buf, 512);
    if (log_size == 0)
    {
        DEBUG_PRINT(("[DEBUG] ftl_restore, no log ...\n"));
        DEBUG_PRINT(("[DEBUG] ftl_restore end\n"));

        return 0;
    }

    /*** Restore IVC, L2P ***/
    // restore number of SLC blocks
    int num_slcBlock = log_buf[212];
    int num_blockWriteOrder = get_num_of_linkedList(ll_head_blockWriteOrder);

    // we should first restore ll_blockWriteOrder, which will then be used to recover L2P, IVC table
    // log_buf[0: 51] stored the value the block order
    int idx = 0, i = 0;
    Node *prev_node = NULL; // = (Node *)malloc(sizeof(Node)); // initialize a dummy node first
    for (idx = 0; idx < 50; idx++)
    {
        int block = log_buf[idx];
        if (block == 255)
            break;
        // DEBUG_PRINT(("[DEBUG] ftl_restore, restore ll_blockWriteOrder %d-th, with block %d\n", idx, block));

        Node *new_node = (Node *)malloc(sizeof(Node));
        new_node->block = block;
        new_node->next = NULL;

        if (idx >= num_blockWriteOrder - num_slcBlock)
        {
            DEBUG_PRINT(("[DEBUG] ftl_restore, restore block %ld in SLC\n", block));
            SLC[block] = 1;
        }

        if (prev_node == NULL)
        {
            ll_head_blockWriteOrder = new_node;
            prev_node = new_node;
        }
        else
        {
            prev_node->next = new_node;
            prev_node = prev_node->next;
        }
    }
    print_linkedList(ll_head_blockWriteOrder);

    // next, we read the spare data from nand, and recover it to L2P in the order described by ll_blockOrder
    //  at the same time, we can recover IVC table by checking whether L2P slot is overwritten
    Node *curr_node = ll_head_blockWriteOrder;
    int *prev_block = (int *)malloc(sizeof(int) * LBA_NUM);
    memset(prev_block, -1, sizeof(int) * LBA_NUM);
    while (curr_node)
    {
        // read the spare data of all pages at current block
        int page = 0;
        for (page = 0; page < LBA_NUM_PER_BLOCK; page++)
        {
            // Read from the spare data from the page of the block to get lba
            PCA_RULE my_pca;
            my_pca.fields.block = curr_node->block;
            my_pca.fields.page = page;

            if (SLC[curr_node->block] == 1 && my_pca.fields.page >= LBA_NUM_PER_BLOCK / 2)
            {
                break;
            }

            size_t lba = spare_read(my_pca.pca);

            // check the last inserted block, if it not full, then we should restore the curr_pca to the last page on it
            if (curr_node->next == NULL && lba == LBA_NUM)
            {
                curr_pca.fields.block = curr_node->block;
                curr_pca.fields.page = page - 1;
                if (curr_pca.fields.page < 0)
                {
                    curr_pca.pca = INVALID_PCA;
                }
                break;
            }

            //
            unusedBlock[curr_node->block] = 0;
            // DEBUG_PRINT(("[DEBUG] ftl_restore, set unusedBlock[%d] to 0\n", curr_node->block));

            // if lba is already occupied, it means that there exist a invalid slot in the previous block
            if (L2P[lba] != INVALID_PCA)
            {
                IVC[prev_block[lba]] += 1;
            }
            prev_block[lba] = curr_node->block;

            // update the pca on L2P table
            L2P[lba] = my_pca.pca;
        }

        curr_node = curr_node->next;
    }

    /*** Restore ERC ***/
    // log_buf[50: 101] stored the value the ERC table
    for (idx = 0; idx < 50; idx++)
    {
        ERC[idx] = log_buf[50 + idx];
        DEBUG_PRINT(("[DEBUG] ftl_restore, restore ERC [%2d] = %d\n", idx, ERC[idx]));
    }

    /*** Restore WAF ***/
    // Restore physical size, host_size, etc.
    for (idx = 0; idx < 3 * 4; idx += 3)
    {
        //
        size_t size = 0;
        size = log_buf[100 + idx] * 65536 + log_buf[100 + idx + 1] * 256 + log_buf[100 + idx + 2];
        DEBUG_PRINT(("[DEBUG] ftl_restore, restore WAF [%2d] = %u\n", idx / 3, size));
        if (idx == 0)
            physic_size = size;
        else if (idx / 3 == 1)
            logic_size = size;
        else if (idx / 3 == 2)
            host_write_size = size;
        else if (idx / 3 == 3)
            nand_write_size = size;
    }

    /*** Restore erasedSlot, and L2P ***/
    int value = -1, slot = -1;
    for (idx = 0; idx < 100; idx++)
    {
        value = log_buf[112 + idx];
        for (i = 0; i < 8; i++)
        {
            slot = (idx + 1) * 8 - i - 1;
            erasedSlot[slot] = value % 2;
            if (erasedSlot[slot] == 1)
            {
                DEBUG_PRINT(("[DEBUG] ftl_restore, restore erasedSlot [%2d] = %d\n", slot, erasedSlot[slot]));

                // erasedSlot is 1, set the L2P[slot] to INVALID_PCA, and IVC[slot] increase by 1
                PCA_RULE my_pca;
                my_pca.pca = L2P[slot];
                IVC[my_pca.fields.block] += 1;
                L2P[slot] = INVALID_PCA;
            }
            value = value / 2;
        }
    }

    DEBUG_PRINT(("[DEBUG] ftl_restore end\n"));

    return 1;
}

int nand_read_log(char *logBuf, int size) // logBuf is output
{
    if (size > 512)
        return;
    FILE *fileResult;
    size_t numWritten;

    fileResult = fopen(LOG_LOCATION, "r");
    if (fileResult == NULL)
    {
        printf("fopen() failed with following error, %s\n", strerror(errno));
        return 0;
    }

    numWritten = fread(logBuf, sizeof(unsigned char), size, fileResult);
    printf("read %zu bytes\n", numWritten);
    fclose(fileResult);
    return numWritten;
}

static int nand_read(char *data_buf, char *spare_buf, int pca, unsigned char mode)
{
    char nand_name[100];
    FILE *fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    char *buf_spare;

    buf_spare = calloc((PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), sizeof(char));

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // read
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, 0L, SEEK_END);
        // calculating the size of the file
        unsigned int fileSize = ftell(fptr);
        // closing the file

        if (my_pca.fields.page * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) >= fileSize)
        {
            memset(data_buf, 0xff, 512);
            memset(spare_buf, 0xff, 8);
        }
        else
        {
            fseek(fptr, my_pca.fields.page * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), SEEK_SET);
            fread(buf_spare, 1, (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), fptr);
            char *check_spare = calloc((PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));
            fseek(fptr, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, SEEK_SET);
            fread(check_spare, 1, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, fptr);
            if ((check_spare[0] & MLC_mode) != mode)
            {
                printf("5A different mode\n");
                printf("this block mode is %d, and read mode is %d\n", (check_spare[0] & MLC_mode), mode);
                memset(data_buf, 0x5A, 512);
                memset(spare_buf, 0x5A, 8);
            }
            else
            {
                if (mode == MLC_mode)
                {
                    if (fileSize != (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PAGE_NUMBER_PER_NAND)
                    {
                        printf("5A MLC not full\n");
                        memset(data_buf, 0x5A, 512);
                        memset(spare_buf, 0x5A, 8);
                    }
                    else if (my_pca.fields.page > PAGE_NUMBER_PER_NAND - 1)
                    {
                        printf("5A MLC exceed page\n");
                        memset(data_buf, 0x5A, 512);
                        memset(spare_buf, 0x5A, 8);
                    }
                    else
                    {
                        memcpy(data_buf, buf_spare, 512);
                        memcpy(spare_buf, buf_spare + 512, 8);
                    }
                }
                else
                {
                    if (my_pca.fields.page > SLC_PAGE_NUMBER_PER_NAND - 1)
                    {
                        printf("5A SLC exceed page\n");
                        memset(data_buf, 0x5A, 512);
                        memset(spare_buf, 0x5A, 8);
                    }
                    else
                    {
                        memcpy(data_buf, buf_spare, 512);
                        memcpy(spare_buf, buf_spare + 512, 8);
                    }
                }
            }
            free(check_spare);
        }
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        free(buf_spare);
        return -EINVAL;
    }
    free(buf_spare);
    return 512;
}

size_t spare_read(unsigned int pca)
{
    /*** Giving PCA, return LBA stored in its spare data ***/
    // DEBUG_PRINT(("[DEBUG] spare_read(pca) from pca %u \n", pca));

    // return 0 if that slot did not have any data there
    if (pca == INVALID_PCA)
    {
        return 0;
    }

    // Allocate space for spare buffer
    char *buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    unsigned char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));

    // Call nand_read and store value inside the buffers (spare_buf)
    unsigned int mode = MLC_mode;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    if (SLC[my_pca.fields.block] == 1)
    {
        mode = SLC_mode;
        DEBUG_PRINT(("[DEBUG] spare_read, read block %ld in SLC mode\n", my_pca.fields.block));
    }

    int rst = nand_read(buf, spare_buf, pca, mode);
    if (rst == -EINVAL)
    {
        return -EINVAL;
    }

    // Handle when the block is not written full
    size_t is_exist_spare_data = spare_buf[0];
    if (is_exist_spare_data != 1)
    {
        // return LBA_NUM when the spare data is not exist
        return LBA_NUM;
    }

    size_t lba = spare_buf[1] * 256 + spare_buf[2];
    // DEBUG_PRINT(("[DEBUG] spare_read(pca) get lba =  %ld \n", lba));
    return lba;
}

static int nand_write(const char *data_buf, const char *spare_buf, int pca, unsigned char mode)
{
    char nand_name[100];
    FILE *fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    char *tmp_spare;
    char *buf_spare;
    buf_spare = calloc((PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), sizeof(char));
    tmp_spare = calloc((PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));
    memcpy(buf_spare, data_buf, 512);
    memcpy(tmp_spare, spare_buf, 8);
    if (spare_buf != NULL)
    {
        tmp_spare[0] = mode;
        memcpy(buf_spare + 512, tmp_spare, 8);
    }
    else
        memset(buf_spare + 512, 0, 8);
    free(tmp_spare);
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // write
    if ((fptr = fopen(nand_name, "r+")))
    {
        if (my_pca.fields.page != 0)
        {
            char *check_spare = calloc((PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));
            fseek(fptr, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, SEEK_SET);
            fread(check_spare, 1, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, fptr);
            if ((check_spare[0] & MLC_mode) == mode)
            {
                if (mode == MLC_mode)
                {
                    if (my_pca.fields.page > PAGE_NUMBER_PER_NAND - 1)
                    {
                        printf("MLC page:%d!\n", my_pca.fields.page);
                        printf("writing wrong page number!\n");
                        free(buf_spare);
                        free(check_spare);
                        fclose(fptr);
                        return -EINVAL;
                    }
                    else
                    {
                        fseek(fptr, my_pca.fields.page * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), SEEK_SET);
                        fwrite(buf_spare, 1, (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), fptr);
                        fclose(fptr);
                        physic_size++;
                    }
                }
                else
                {
                    if (my_pca.fields.page > SLC_PAGE_NUMBER_PER_NAND - 1)
                    {
                        printf("SLC page:%d!\n", my_pca.fields.page);
                        printf("writing wrong page number!\n");
                        free(buf_spare);
                        free(check_spare);
                        fclose(fptr);
                        return -EINVAL;
                    }
                    else
                    {
                        fseek(fptr, my_pca.fields.page * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), SEEK_SET);
                        fwrite(buf_spare, 1, (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), fptr);
                        fclose(fptr);
                        physic_size++;
                    }
                }
            }
            else
            {
                printf("this block mode is %d, and write mode is %d\n", (check_spare[0] & MLC_mode), mode);
                free(buf_spare);
                free(check_spare);
                fclose(fptr);
                return -EINVAL;
            }
        }
        else
        {
            fseek(fptr, my_pca.fields.page * (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), SEEK_SET);
            fwrite(buf_spare, 1, (PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE), fptr);
            fclose(fptr);
            physic_size++;
        }

        // nand_table[my_pca.fields.block].valid_cnt++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        free(buf_spare);
        return -EINVAL;
    }
    free(buf_spare);
    nand_write_size += 512;
    return 512;
}

static int nand_erase(int nand);
static unsigned int get_next_pca();
void ftl_do_copyback_helper(int block)
{
    DEBUG_PRINT(("[DEBUG] ftl_do_copyback, with block %d\n", block));
    int page_idx = -1;
    for (page_idx = 0; page_idx < LBA_NUM_PER_BLOCK / 2; page_idx++)
    {
        //
        PCA_RULE my_pca;
        my_pca.fields.block = block;
        my_pca.fields.page = page_idx;

        // read from a slc block
        // Call nand_read and store value inside the buffers (spare_bug & buf)
        char *tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        char *tmp_spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
        int rst = nand_read(tmp_buf, tmp_spare_buf, my_pca.pca, SLC_mode);
        if (rst == -EINVAL)
            return -EINVAL;

        // write to a mlc block
        unsigned int pca = get_next_pca();
        size_t lba = (unsigned char)tmp_spare_buf[1] * 256 + (unsigned char)tmp_spare_buf[2];
        tmp_spare_buf[0] = MLC_mode & 1;
        rst = nand_write(tmp_buf, tmp_spare_buf, pca, MLC_mode);
        if (rst == -EINVAL)
            return -EINVAL;

        free(tmp_buf);
        free(tmp_spare_buf);

        // deal with the case when erasedSlot[lba] = 1
        // when erasedSlot[lba] = 1, we should not need to replace the L2P
        if (erasedSlot[lba] != 1)
        {
            unsigned int pre_pca = L2P[lba];
            if (pre_pca != INVALID_PCA)
            {
                // Noted that when the slot was not empty, we need to update the IVC (collect there has a invalid page in that block)
                my_pca.pca = pre_pca;
                IVC[my_pca.fields.block] += 1;
            }
            DEBUG_PRINT(("ftl_do_copyback_helper, replace lba %u to a copyback version, from old pca %d -> new pca %u\n", lba, pre_pca, pca));
            L2P[lba] = pca;
            erasedSlot[lba] = 0;
        }
        else
        {
            DEBUG_PRINT(("ftl_do_copyback_helper, erased, replace lba %u to a copyback version, bad pca %u\n", lba, pca));
        }
    }

    // TODO Remove from ll_blockWriteOrder is enough
    nand_erase(block);
    ll_head_unusedBlock = delete_node_from_linkedList(ll_head_unusedBlock, block);
    ll_head_unusedBlock = append_node_to_linkedList(ll_head_unusedBlock, block);

    SLC[block] = 0;
}

void ftl_do_copyback()
{
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ftl_do_copyback start\n"));

    //
    int num_slcBlock = 0;
    int slcBlock0 = -1, slcBlock1 = -1;
    int block_idx = -1;
    for (block_idx = 0; block_idx < PHYSICAL_NAND_NUM; block_idx++)
    {
        if (SLC[block_idx] != 0)
        {
            if (SLC[block_idx] == 1)
            {
                DEBUG_PRINT(("[DEBUG] ftl_do_copyback find 1st slc block %d with SLC is %d\n", block_idx, SLC[block_idx]));
                slcBlock0 = block_idx;
            }
            else
            {
                DEBUG_PRINT(("[DEBUG] ftl_do_copyback find 2nd slc block %d with SLC is %d\n", block_idx, SLC[block_idx]));
                slcBlock1 = block_idx;
            }

            num_slcBlock++;
        }
    }

    //
    if (num_slcBlock < 2)
    {
        DEBUG_PRINT(("[DEBUG] ftl_do_copyback ends, with no enough slc block %d\n", num_slcBlock));
        return;
    }

    //
    ftl_do_copyback_helper(slcBlock0);
    ftl_do_copyback_helper(slcBlock1);

    DEBUG_PRINT(("[DEBUG] ftl_do_copyback end\n"));
    DEBUG_PRINT(("=====================================================\n"));
}

static int nand_erase(int nand)
{
    char nand_name[100];
    int found = 1;
    FILE *fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, nand);

    // erase
    if ((fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, nand, -EINVAL);
        return -EINVAL;
    }

    if (found == 0)
    {
        printf("nand erase not found\n");
        return -EINVAL;
    }

    /*** FTL-maintain ***/
    /*** update linked list ***/
    ll_head_blockWriteOrder = delete_node_from_linkedList(ll_head_blockWriteOrder, nand);
    // ll_head_unusedBlock = delete_node_from_linkedList(ll_head_unusedBlock, nand);
    // ll_head_unusedBlock = append_node_to_linkedList(ll_head_unusedBlock, nand);

    /*** update table ***/
    // TODO should we reset erasedSlot table during nand_erase
    ERC[nand]++;
    IVC[nand] = 0;
    SLC[nand] = 0;

    /*** update log ***/
    flag_updateLog = 1;

    printf("nand erase %d pass\n", nand);
    return 1;
}

static unsigned int get_next_pca()
{

    DEBUG_PRINT(("[DEBUG] get_next_pca start\n"));

    if (curr_pca.pca == INVALID_PCA)
    {
        // init
        DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d\n", ll_head_unusedBlock->block));
        int rst = nand_erase(ll_head_unusedBlock->block);
        if (rst == -EINVAL)
            return -EINVAL;
        curr_pca.fields.block = ll_head_unusedBlock->block;
        curr_pca.fields.page = 0;
        printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);

        /*** update linked list ***/
        // cleanBlock is now lower by 1
        Node *tmp = ll_head_unusedBlock;
        ll_head_unusedBlock = ll_head_unusedBlock->next;
        free(tmp);
        DEBUG_PRINT(("[DEBUG] get_next_pca, update the unusedBlock\n"));
        print_linkedList(ll_head_unusedBlock);

        // and blockWriteOrder should be updated
        ll_head_blockWriteOrder = delete_node_from_linkedList(ll_head_blockWriteOrder, curr_pca.fields.block);
        ll_head_blockWriteOrder = append_node_to_linkedList(ll_head_blockWriteOrder, curr_pca.fields.block);
        DEBUG_PRINT(("[DEBUG] get_next_pca, update the blockWriteOrder\n"));
        print_linkedList(ll_head_blockWriteOrder);

        /*** update table ***/
        /*** update log ***/
        flag_updateLog = 1;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        // full ssd, no pca can allocate
        printf("No new PCA\n");
    }

    else if (curr_pca.fields.page >= LBA_NUM_PER_BLOCK - 1)
    {
        // if there exists no more clean block
        if (ll_head_unusedBlock == NULL)
        {
            printf("No new PCA\n");
            curr_pca.pca = FULL_PCA;
        }
        else
        {
            /*** update curr_pca ***/
            DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d\n", ll_head_unusedBlock->block));
            int rst = nand_erase(ll_head_unusedBlock->block);
            if (rst == -EINVAL)
                return -EINVAL;
            curr_pca.fields.block = ll_head_unusedBlock->block;
            curr_pca.fields.page = 0;
            printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);

            /*** update linked list ***/
            // cleanBlock is now lower by 1
            Node *tmp = ll_head_unusedBlock;
            ll_head_unusedBlock = ll_head_unusedBlock->next;
            free(tmp);
            DEBUG_PRINT(("[DEBUG] get_next_pca, update the unusedBlock\n"));
            print_linkedList(ll_head_unusedBlock);

            // and blockWriteOrder should be updated
            ll_head_blockWriteOrder = delete_node_from_linkedList(ll_head_blockWriteOrder, curr_pca.fields.block);
            ll_head_blockWriteOrder = append_node_to_linkedList(ll_head_blockWriteOrder, curr_pca.fields.block);
            DEBUG_PRINT(("[DEBUG] get_next_pca, update the blockWriteOrder\n"));
            print_linkedList(ll_head_blockWriteOrder);

            /*** update table ***/
            /*** update log ***/
            flag_updateLog = 1;
        }
    }
    else
    {
        // still inside a block, forward page by one
        curr_pca.fields.page++;
        printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    }

    return curr_pca.pca;
}

static CacheEntry *cache_read(char *data_buf, int logic_lba)
{
    // DEBUG_PRINT(("[DEBUG] cache_read start\n"));
    // // 1 for data exist, 0 for no data (not in cache, or the latest data in cache is erased)
    // 1 for lba exist in cache (including when erasedSlot = 1), 0 for no in cache

    //
    CacheEntry *res = NULL;
    CacheEntry *curr_node = ll_head_cache;
    while (curr_node != NULL)
    {
        if (curr_node->lba == logic_lba)
        {
            if (curr_node->erasedSlot == 1)
            {
                // erased
                DEBUG_PRINT(("[DEBUG] cache_read read lba %ld with erased\n", logic_lba));
                memset(data_buf, 0, sizeof(char) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
                res = curr_node;
            }
            else
            {
                DEBUG_PRINT(("[DEBUG] cache_read read lba %ld  \n", logic_lba));
                memcpy(data_buf, curr_node->data, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
                res = curr_node;
            }
        }
        curr_node = curr_node->next;
    }

    // DEBUG_PRINT(("[DEBUG] cache_read end\n"));
    return res;
}

static int ftl_flush();
static int cache_write(CacheEntry *new_entry)
{
    DEBUG_PRINT(("[DEBUG] cache_write start\n"));

    //
    // CacheEntry *curr_node, *prev_node = NULL;
    int num_cacheEntries = get_num_of_cache(ll_head_cache);

    // If cache is not full, add a new entry to the end of the list
    if (num_cacheEntries < CACHE_BUFFER_SIZE)
    {
        DEBUG_PRINT(("[DEBUG] cache_write, with adding a new entry to lba %u\n", new_entry->lba));

        //
        ll_head_cache = append_node_to_cache(ll_head_cache, new_entry);
        print_cache(ll_head_cache);
        DEBUG_PRINT(("[DEBUG] cache_write, cache_write success\n"));
        return 1;
    }

    else
    {
        // if cache is full, flush the oldest entry (head of the list), add a new one to the end
        DEBUG_PRINT(("[DEBUG] cache_write, with flush the oldest entry, and write lba %u\n", new_entry->lba));

        // flush the first cache entry into nand, and update the ll_head_cache
        CacheEntry *popped_node = ll_head_cache;
        int rst = ftl_flush(popped_node);
        if (rst == -EINVAL)
            return -EINVAL;
        ll_head_cache = ll_head_cache->next;
        free(popped_node);

        //
        ll_head_cache = append_node_to_cache(ll_head_cache, new_entry);
        print_cache(ll_head_cache);
        DEBUG_PRINT(("[DEBUG] cache_write, cache_write success\n"));
        return 1;
    }

    DEBUG_PRINT(("[DEBUG] cache_write end\n"));
    return 0;
}

static int ftl_gc();
static int ftl_write(const char *buf, size_t lba_range, size_t lba);
static int ftl_flush(CacheEntry *popped_node)
{
    DEBUG_PRINT(("[DEBUG] ftl_flush start\n"));

    // gc
    ftl_gc();

    // call nand_write to write the buffer to the NAND
    if (popped_node->erasedSlot == 0)
    {
        //
        ftl_write(popped_node->data, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, popped_node->lba);
    }
    else
    {
        // If erase is aligned, just erase from L2P is enough, and update the log
        DEBUG_PRINT(("[DEBUG] ftl_flush, set erasedSlot[%3d] to 1\n", popped_node->lba));
        unsigned int pca = L2P[popped_node->lba];
        L2P[popped_node->lba] = INVALID_PCA;

        if (pca != INVALID_PCA)
        {
            PCA_RULE my_pca;
            my_pca.pca = pca;
            IVC[my_pca.fields.block] += 1;
        }

        if (erasedSlot[popped_node->lba] != 1)
            flag_updateLog = 1;
        erasedSlot[popped_node->lba] = 1;
    }

    //
    ftl_write_log();

    DEBUG_PRINT(("[DEBUG] ftl_flush end\n"));
    return 0;
}

static int ftl_read(char *buf, size_t lba)
{
    // get pca from L2P table
    unsigned int pca = L2P[lba];
    DEBUG_PRINT(("[DEBUG] ftl_read from LBA %ld -> PCA %u\n", lba, pca));

    // return 0 if that slot did not have any data there
    if (pca == INVALID_PCA)
        return 0;

    // allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    // and call nand_read and store value inside the buffers (spare_bug & buf)

    unsigned int mode = MLC_mode;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    if (SLC[my_pca.fields.block] != 0)
    {
        mode = SLC_mode;
        DEBUG_PRINT(("[DEBUG] ftl_read, read block %d in SLC mode\n", my_pca.fields.block));
    }

    int rst = nand_read(buf, spare_buf, pca, mode);

    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    return rst;
}

static int ftl_write(const char *buf, size_t lba_range, size_t lba)
{

    DEBUG_PRINT(("[DEBUG] ftl_write start\n"));

    // //
    // if (is_gc == 0)
    // {
    //     int rst = cache_write(buf, lba);
    //     return rst;
    // }

    // get pca via calling get_next_pca()
    unsigned int pca = get_next_pca();

    PCA_RULE my_pca;
    my_pca.pca = pca;
    if (my_pca.fields.page >= LBA_NUM_PER_BLOCK / 2)
    {
        DEBUG_PRINT(("[DEBUG] ftl_write, block %ld has page %ld, switch to a new SLC block\n", my_pca.fields.block, my_pca.fields.page));
        curr_pca.fields.page = LBA_NUM_PER_BLOCK;
        // pca = get_next_pca();
        // my_pca.pca = pca;
        ftl_do_copyback();
        // DEBUG_PRINT(("[DEBUG] ftl_write, block %ld is in the slcBlock\n", my_pca.fields.block));

        //
        pca = get_next_pca();
    }

    DEBUG_PRINT(("[DEBUG] ftl_write from LBA %ld -> PCA %u\n", lba, pca));

    // allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(spare_buf, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE * sizeof(char));
    unsigned char state = 1;
    sprintf(spare_buf, "%c%c%c", state, lba / 256, lba % 256);
    // sprintf(spare_buf, 3, "%c%c%c", state, lba / 256, lba % 256);

    // call nand_write to write the buffer to the NAND
    int rst = nand_write(buf, spare_buf, pca, SLC_mode);
    if (rst == -EINVAL)
        return -EINVAL;
    DEBUG_PRINT(("[DEBUG] ftl_write, nand_write success\n"));

    /*** update linked list ***/
    /*** update table ***/
    // if nand_write complete successfully, then update the L2P table
    unsigned int pre_pca = L2P[lba];
    if (pre_pca != INVALID_PCA)
    {
        // Noted that when the slot was not empty, we need to update the IVC (collect there has a invalid page in that block)
        my_pca.pca = pre_pca;
        IVC[my_pca.fields.block] += 1;
    }
    L2P[lba] = pca;
    if (erasedSlot[lba] != 0)
        flag_updateLog = 1;
    erasedSlot[lba] = 0;

    my_pca.pca = pca;

    if (SLC[my_pca.fields.block] == 0)
    {
        // the first SLC[] of first SLC block should be 1, the second for 2
        int idx = 0;
        int is_exist_fst_slcBlock = 0;
        for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        {
            if (SLC[idx] == 1)
            {
                is_exist_fst_slcBlock = 1;
                break;
            }
        }

        if (is_exist_fst_slcBlock == 0)
            SLC[my_pca.fields.block] = 1;
        else
            SLC[my_pca.fields.block] = 2;
    }

    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] ftl_write end\n"));

    return rst;
}

static int ftl_gc()
{
    /*** check whether need to do gc ***/
    int num_unusedBlock = get_num_of_linkedList(ll_head_unusedBlock);
    DEBUG_PRINT(("[DEBUG] num_unusedBlock is %d\n", num_unusedBlock));
    // print_linkedList(ll_head_unusedBlock);

    unsigned int *unusedBlock;
    unusedBlock = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    // memset(unusedBlock, 0, sizeof(int) * PHYSICAL_NAND_NUM);
    int idx = 0;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        unusedBlock[idx] = 0;

    DEBUG_PRINT(("ll_head_unusedBlock : "));
    Node *curr_node = ll_head_unusedBlock;
    while (curr_node)
    {
        DEBUG_PRINT(("%d -> ", curr_node->block));
        unusedBlock[curr_node->block] = 1;
        curr_node = curr_node->next;
    }
    DEBUG_PRINT(("\n"));

    if (num_unusedBlock > 5)
    {
        DEBUG_PRINT(("[DEBUG] break from gc, we still have enough blocks\n"));
        free(unusedBlock);
        return 0;
    }

    DEBUG_PRINT(("[DEBUG] ftl_gc start\n"));

    /*** decide the source block to be erased ***/
    int idx_badBlock = -1, num_HighestIVC = 0;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        // DEBUG_PRINT(("[DEBUG] ftl_gc, unusedBlock[%2d] is %d\n", idx, unusedBlock[idx]));
        if (unusedBlock[idx] == 1)
            continue;
        DEBUG_PRINT(("[DEBUG] ftl_gc, IVC[%2d] = %d\n", idx, IVC[idx]));
        if (IVC[idx] > num_HighestIVC)
        {
            num_HighestIVC = IVC[idx];
            idx_badBlock = idx;
        }
    }
    free(unusedBlock);
    DEBUG_PRINT(("[DEBUG] ftl_gc, select %d as source block to erase, which IVC is %d\n", idx_badBlock, num_HighestIVC));

    /*** move all the valid data that in source block to another block ***/
    unsigned int is_valid_data[LBA_NUM_PER_BLOCK];
    memset(is_valid_data, INVALID_PCA, LBA_NUM_PER_BLOCK * sizeof(unsigned int));
    int num_validPage = 0;
    for (idx = 0; idx < LBA_NUM; idx++)
    {
        unsigned int pca = L2P[idx];
        PCA_RULE my_pca;
        my_pca.pca = pca;
        if (my_pca.fields.block == idx_badBlock)
        {
            is_valid_data[my_pca.fields.page] = idx;
            num_validPage++;
            DEBUG_PRINT(("[DEBUG] ftl_gc, is_valid_data[%u] = %d\n", my_pca.fields.page, idx));
        }
    }
    DEBUG_PRINT(("[DEBUG] ftl_gc, number of valid data in block: %d\n", num_validPage));

    /*** update L2P table ***/
    int rst = 0;
    for (idx = 0; idx < LBA_NUM_PER_BLOCK; idx++)
    {
        if (is_valid_data[idx] == INVALID_PCA)
            continue;

        // read the buffer that stored in the bad block
        int lba = is_valid_data[idx];
        char *tmp_buf = malloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * sizeof(char));
        rst = ftl_read(tmp_buf, lba);
        if (rst == -EINVAL)
            return -EINVAL;

        // and write it back to a new pca
        rst = ftl_write(tmp_buf, 1, lba);
        if (rst == -EINVAL)
            return -EINVAL;
    }
    DEBUG_PRINT(("[DEBUG] ftl_gc, move all the valid data into new pca\n"));

    /*** erase the source block with invalid data ***/
    // rst = nand_erase(idx_badBlock);
    // if (rst == -EINVAL)
    //     return -EINVAL;
    ll_head_blockWriteOrder = delete_node_from_linkedList(ll_head_blockWriteOrder, idx_badBlock);
    ll_head_unusedBlock = delete_node_from_linkedList(ll_head_unusedBlock, idx_badBlock);
    ll_head_unusedBlock = append_node_to_linkedList(ll_head_unusedBlock, idx_badBlock);
    DEBUG_PRINT(("[DEBUG] ftl_gc, erase block success\n"));

    flag_updateLog = 1;

    DEBUG_PRINT(("[DEBUG] ftl_gc end\n"));
    return 0;
}

static int ssd_file_type(const char *path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

static int ssd_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    (void)fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
    case SSD_ROOT:
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        break;
    case SSD_FILE:
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = logic_size;
        break;
    case SSD_NONE:
        return -ENOENT;
    }
    return 0;
}

static int ssd_open(const char *path, struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

static int ssd_do_read(char *buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst;
    char *tmp_buf;

    // off limit
    if ((offset) >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        // is valid data section
        size = logic_size - offset;
    }

    // divide read cmd into 512B package by size
    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    DEBUG_PRINT(("[DEBUG] ssd_do_read, with size %ld, offset %ld ==> LBA from %d -> %d\n", size, offset, tmp_lba, tmp_lba_range + tmp_lba - 1));

    tmp_buf = calloc(tmp_lba_range * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        // use cache_read & ftl_read to read data and error handling
        // rst = cache_read(tmp_buf + i * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, tmp_lba + i);
        int rst = 0;

        CacheEntry *entry = cache_read(tmp_buf + i * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, tmp_lba + i);
        if (entry == NULL)
            rst = ftl_read(tmp_buf + i * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, tmp_lba + i);

        if (rst == 0)
        {
            // no data at the current lba
            continue;
        }
        else if (rst == -EINVAL)
        {
            // failed case
            return -EINVAL;
        }
    }

    memcpy(buf, tmp_buf + offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, size);
    free(tmp_buf);
    return size;
}

static int ssd_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ssd_read, with size %ld, offset %ld\n", size, offset));
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    int rst = ssd_do_read(buf, size, offset);
    DEBUG_PRINT(("[DEBUG] ssd_read end\n"));
    DEBUG_PRINT(("=====================================================\n"));
    return rst;
}

void ssd_do_flush()
{
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ssd_do_flush start\n"));

    // TODO When flushing the whole cache, only flush the latest entry of the same lba
    CacheEntry *curr_node = ll_head_cache;
    while (curr_node != NULL)
    {
        ftl_flush(curr_node);
        CacheEntry *tmp = curr_node;
        curr_node = curr_node->next;
        free(tmp);
    }
    ll_head_cache = NULL;

    DEBUG_PRINT(("[DEBUG] ssd_do_flush end\n"));
    DEBUG_PRINT(("=====================================================\n"));
}

static int ssd_do_write(const char *buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, curr_offset, remain_size;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // divide write-in value into 512B package by size, processing them sequencial
    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    DEBUG_PRINT(("[DEBUG] ssd_do_write, with size %ld, offset %ld ==> LBA from %d -> %d\n", size, offset, tmp_lba, tmp_lba_range + tmp_lba - 1));

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        // always check whether the gc is needed, and do gc when it is needed
        ftl_gc();

        // tmp_buf is the current 512B write-in value
        char *tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        memset(tmp_buf, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

        // int rst = cache_read(tmp_buf, tmp_lba + idx);
        int rst = 0;
        CacheEntry *entry = cache_read(tmp_buf, tmp_lba + idx);
        if (entry == NULL)
            rst = ftl_read(tmp_buf, tmp_lba + idx);

        if (rst == -EINVAL)
            return -EINVAL;

        // handling non-aligned data
        // if we met a non-aligned write-in date, then the first and the last 512B data need to be handled differently
        curr_offset = 0;
        if (idx == 0)
        {
            curr_offset = offset - (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
            curr_size = (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + 1) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - offset;
            curr_size = curr_size > remain_size ? remain_size : curr_size;
        }
        else if (idx == tmp_lba_range - 1)
            curr_size = (remain_size);
        else
            curr_size = PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;

        memcpy(tmp_buf + curr_offset, buf + process_size, curr_size);
        DEBUG_PRINT(("[DEBUG] ssd_do_write, fill from offset %d to %d\n", curr_offset, curr_offset + curr_size));

        // rst = ftl_write(tmp_buf, curr_size, tmp_lba + idx);

        CacheEntry *new_entry = malloc(sizeof(CacheEntry));
        new_entry->lba = tmp_lba + idx;
        new_entry->erasedSlot = 0;
        memcpy(new_entry->data, tmp_buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        new_entry->next = NULL;

        rst = cache_write(new_entry);
        if (rst == -EINVAL)
            return -EINVAL;
        free(tmp_buf);

        // update the process_, remain_, curr_ size
        process_size += curr_size;
        remain_size -= curr_size;
    }

    ftl_write_log();

    return size;
}

static int ssd_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ssd_write start\n"));
    DEBUG_PRINT(("[DEBUG] ssd_write, with size %ld, offset %ld\n", size, offset));
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    int rst = ssd_do_write(buf, size, offset);
    DEBUG_PRINT(("[DEBUG] ssd_write end\n"));
    DEBUG_PRINT(("=====================================================\n"));
    return rst;
}

static int ftl_erase(const char *buf, int lba)
{
    DEBUG_PRINT(("[DEBUG] ftl_erase start\n"));

    // modify the lba mapping value in L2P table into INVALID_PCA, and update IVC table
    if (buf != NULL)
    {
        DEBUG_PRINT(("[DEBUG] ftl_erase, write the modified buffer to nand\n"));
        // ftl_write will already update IVC, don't collect two times
        // int rst = ftl_write(buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, lba);

        CacheEntry *new_entry = malloc(sizeof(CacheEntry));
        new_entry->lba = lba;
        new_entry->erasedSlot = 0;
        memcpy(new_entry->data, buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        new_entry->next = NULL;

        int rst = cache_write(new_entry);
        if (rst == -EINVAL)
            return -EINVAL;
    }
    else
    {
        // if erase is aligned, just erase from L2P is enough, and update the log
        DEBUG_PRINT(("[DEBUG] ftl_erase, set the erasedSlot %d to 1\n", lba));

        //
        CacheEntry *new_entry = malloc(sizeof(CacheEntry));
        new_entry->lba = lba;
        new_entry->erasedSlot = 1;
        // memcpy(new_entry->data, data, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        new_entry->next = NULL;

        int rst = cache_write(new_entry);
        return rst;

        /*** update linked list ***/
        /*** update table ***/
        // unsigned int pca = L2P[lba];
        // L2P[lba] = INVALID_PCA;
        // PCA_RULE my_pca;
        // my_pca.pca = pca;
        // IVC[my_pca.fields.block] += 1;
        // if (erasedSlot[lba] != 1)
        //     flag_updateLog = 1;
        // erasedSlot[lba] = 1;

        /*** update log ***/
    }

    DEBUG_PRINT(("[DEBUG] ftl_erase end\n"));

    return 0;
}

static int ssd_do_erase(int offset, int size)
{
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ssd_do_erase start\n"));
    DEBUG_PRINT(("[DEBUG] ssd_do_erase, with size %d, offset %d\n", size, offset));

    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size;
    int curr_offset = 0;
    int rst;

    //
    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    tmp_lba_range = (offset + size - 1) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    DEBUG_PRINT(("[DEBUG] ssd_do_erase, LBA from %d -> %d\n", tmp_lba, tmp_lba_range + tmp_lba - 1));

    //
    process_size = 0;
    remain_size = size;
    curr_size = 0;
    curr_offset = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        // always check whether the gc is needed, and do gc when it is needed
        ftl_gc();

        // handling non-aligned data
        // for non-aligned issue, read the whole page, erase the data, and move the remain data to a new page
        size_t lba = tmp_lba + idx;

        const char *tmp_buf = NULL;
        int prev_offset = offset + process_size;
        curr_offset = prev_offset - (prev_offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
        curr_size = (prev_offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + 1) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - prev_offset;
        curr_size = curr_size > remain_size ? remain_size : curr_size;

        // if L2P[lba] has no PCA, and we also can't find the lba on cache -> no data exists, no need to erase
        tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        memset(tmp_buf, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

        CacheEntry *entry = cache_read(tmp_buf, lba);
        // TODO Can we decrease WAF by ignoring useless command ?
        // if ((entry != NULL && entry->erasedSlot == 1) || (entry == NULL && L2P[lba] == INVALID_PCA))
        // {
        //     process_size += curr_size;
        //     remain_size -= curr_size;
        //     curr_size = 0;
        //     free(tmp_buf);
        //     tmp_buf = NULL;
        //     continue;
        // }
        if (curr_size == PHYSICAL_DATA_SIZE_BYTES_PER_PAGE)
        {
            process_size += curr_size;
            remain_size -= curr_size;
            curr_size = 0;
            free(tmp_buf);
            tmp_buf = NULL;
        }
        else
        {
            if (entry == NULL)
                rst = ftl_read(tmp_buf, lba);
            if (rst == -EINVAL)
                return -EINVAL;
            memset(tmp_buf + curr_offset, 0, curr_size);
        }

        //
        rst = ftl_erase(tmp_buf, lba);
        if (rst == -EINVAL)
            return -EINVAL;

        // update the process_, remain_, curr_ size
        process_size += curr_size;
        remain_size -= curr_size;
        curr_size = 0;
    }

    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/
    ftl_write_log();

    DEBUG_PRINT(("[DEBUG] ssd_do_erase end\n"));
    DEBUG_PRINT(("=====================================================\n"));
    return 0;
}

static int ssd_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

static int ssd_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void)fi;
    (void)offset;
    (void)flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

static int ssd_ioctl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags, void *data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
    case SSD_GET_LOGIC_SIZE:
        *(size_t *)data = logic_size;
        printf(" --> logic size: %ld\n", logic_size);
        return 0;
    case SSD_GET_PHYSIC_SIZE:
        *(size_t *)data = physic_size;
        printf(" --> physic size: %ld\n", physic_size);
        return 0;
    case SSD_GET_WA:
        *(double *)data = (double)nand_write_size / (double)host_write_size;
        return 0;
    case SSD_LOGIC_ERASE:
    {
        unsigned long long eraseFrame;
        eraseFrame = *(unsigned long long *)data;
        int eraseSize = eraseFrame & 0xFFFFFFFF;
        int eraseStart = (eraseFrame >> 32) & 0xFFFFFFFF;
        printf(" --> erase start: %u, erase size: %u\n", eraseStart, eraseSize);
        ssd_do_erase(eraseStart, eraseSize);
        return 0;
    }
    case SSD_FLUSH:
        ssd_do_flush();
        printf(" --> do flush\n");
        return 0;
    case SSD_SHOW_L2P:
    {
        int idx = 0;
        for (idx = 0; idx < LBA_NUM; idx++)
        {
            printf("L2P[%3d] = %u\n", idx, L2P[idx]);
        }
        return 0;
    }
    }
    return -EINVAL;
}

static const struct fuse_operations ssd_oper =
    {
        .getattr = ssd_getattr,
        .readdir = ssd_readdir,
        .truncate = ssd_truncate,
        .open = ssd_open,
        .read = ssd_read,
        .write = ssd_write,
        .ioctl = ssd_ioctl,
};

// -------------------------------------------------------------- //

int main(int argc, char *argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    nand_write_size = 0;
    host_write_size = 0;
    curr_pca.pca = INVALID_PCA;

    /*** Initialize memory for tables ***/
    DEBUG_PRINT(("[DEBUG] main, initialize memory for all table complete\n"));

    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

    IVC = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(IVC, 0, sizeof(int) * PHYSICAL_NAND_NUM);

    ERC = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(ERC, 0, sizeof(int) * PHYSICAL_NAND_NUM);

    erasedSlot = malloc(LBA_NUM * sizeof(int));
    memset(erasedSlot, 0, sizeof(int) * LBA_NUM);

    SLC = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(SLC, 0, sizeof(int) * PHYSICAL_NAND_NUM);

    /*** Restore from log ***/
    unsigned int *unusedBlock;
    unusedBlock = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    // memset(unusedBlock, 0, sizeof(int) * PHYSICAL_NAND_NUM);
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        unusedBlock[idx] = 1;
    int is_exist_log = ftl_restore(unusedBlock);

    /*** Construct the linked list for clean blocks ***/
    DEBUG_PRINT(("[DEBUG] main, construct the ll_head_unusedBlock\n"));
    // Initialize a dummy node at first
    ll_head_unusedBlock = (Node *)malloc(sizeof(Node));
    Node *prev_block = NULL;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        //
        if (unusedBlock[idx] == 0)
            continue;

        // create a new node and append to the linked list
        // DEBUG_PRINT(("[DEBUG] main, build ll_head_unusedBlock with block %d\n", idx));
        Node *new_node = (Node *)malloc(sizeof(Node));
        new_node->block = idx;
        new_node->next = NULL;

        if (prev_block == NULL)
        {
            ll_head_unusedBlock = new_node;
            prev_block = ll_head_unusedBlock;
        }
        else
        {
            prev_block->next = new_node;
            prev_block = new_node;
        }
    }
    print_linkedList(ll_head_unusedBlock);
    free(unusedBlock);

    // create nand file and log file
    if (is_exist_log == 0)
    {
        DEBUG_PRINT(("[DEBUG] main, find no log, create new log and nand files\n"));
        FILE *fptr;
        fptr = fopen(LOG_LOCATION, "w");
        fclose(fptr);

        for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        {
            snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
            fptr = fopen(nand_name, "w");
            if (fptr == NULL)
            {
                printf("open fail");
            }
            fclose(fptr);
        }
    }

    return fuse_main(argc, argv, &ssd_oper, NULL);
}
