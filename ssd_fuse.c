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

int write_buffer_index = 0;
char write_buffer[PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * WRITE_BUFFER_PAGE_NUM];

// A linked list that points blocks together
typedef struct node Node;
struct node
{
    int block;
    struct node *next;
};

/*** Linked List ***/
Node *ll_head_unusedBlock = NULL;
// Node *ll_rear_cleanBlock = NULL;

Node *ll_head_blockWriteOrder = NULL; // the order in which block is written

/*** Flags ***/
int flag_updateLog = 0;

// -------------------------------------------------------------- //
// temp
// an array to help re-construct the linked list of clean blocks
unsigned int *unusedBlock;

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

void ftl_do_copyback()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_do_copyback start\n"));

    // TODO
    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] ftl_do_copyback end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
}

void ftl_write_log()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_write_log start\n"));

    // only write log when the update flag is toggled
    if (flag_updateLog == 1)
    {
        // TODO
        // nand_write_log(/*your log*/, /*your log size*/);
    }

    // reset flags
    flag_updateLog = 0;

    DEBUG_PRINT(("[DEBUG] ftl_write_log end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
}

int ftl_restore()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_restore start\n"));

    // call nand_read_log() to read the log
    unsigned char *log_buf = calloc(512, sizeof(unsigned char));
    size_t log_size = nand_read_log(log_buf, 512);
    if (log_size == 0)
    {
        DEBUG_PRINT(("[DEBUG] ftl_restore, no log ...\n"));
        DEBUG_PRINT(("-----------------------------------------------------\n"));
        return 0;
    }

    DEBUG_PRINT(("[DEBUG] ftl_restore end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
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

static int nand_read(char *data_buf, char *spare_buf, int pca)
{
    char nand_name[100];
    FILE *fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;
    char *tmp_spare = calloc((PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // read
    if ((fptr = fopen(nand_name, "r")))
    {
        fseek(fptr, my_pca.fields.page * (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), SEEK_SET);
        fread(tmp_spare, 1, (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), fptr);
        fclose(fptr);

        memcpy(data_buf, tmp_spare, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
        memcpy(spare_buf, tmp_spare + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    free(tmp_spare);
    return PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
}

static int nand_write(const char *data_buf, const char *spare_buf, int pca) // spare can use NULL
{
    char nand_name[100];
    FILE *fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca;

    char *tmp_spare;
    tmp_spare = calloc((PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), sizeof(char));
    memcpy(tmp_spare, data_buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);
    if (spare_buf != NULL)
        memcpy(tmp_spare + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, spare_buf, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);
    else
        memset(tmp_spare + PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE);

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // write
    if ((fptr = fopen(nand_name, "r+")))
    {
        fseek(fptr, my_pca.fields.page * (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), SEEK_SET);
        fwrite(tmp_spare, 1, (PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE), fptr);
        fclose(fptr);
        physic_size++;
        // nand_table[my_pca.fields.nand].valid_cnt++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }
    nand_write_size += PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    free(tmp_spare);
    return PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
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
    Node *curr_block = head;
    DEBUG_PRINT(("linkedList : "));
    while (curr_block != NULL)
    {
        DEBUG_PRINT(("%d -> ", curr_block->block));
        // forward to next node
        curr_block = curr_block->next;
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
    ll_head_unusedBlock = append_node_to_linkedList(ll_head_unusedBlock, nand);

    /*** update table ***/
    ERC[nand]++;
    IVC[nand] = 0;

    /*** update log ***/
    flag_updateLog = 1;

    printf("nand erase %d pass\n", nand);
    return 1;
}

static unsigned int get_next_pca()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] get_next_pca start\n"));

    if (curr_pca.pca == INVALID_PCA)
    {
        // init
        print_linkedList(ll_head_unusedBlock);
        DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d\n", ll_head_unusedBlock->block));
        curr_pca.fields.block = ll_head_unusedBlock->block;
        curr_pca.fields.page = 0;
        printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);

        /*** update linked list ***/
        // cleanBlock is now lower by 1
        Node *tmp = ll_head_unusedBlock;
        ll_head_unusedBlock = ll_head_unusedBlock->next;
        free(tmp);

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
            print_linkedList(ll_head_unusedBlock);
            DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d\n", ll_head_unusedBlock->block));
            curr_pca.fields.block = ll_head_unusedBlock->block;
            curr_pca.fields.page = 0;
            printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);

            /*** update linked list ***/
            // cleanBlock is now lower by 1
            Node *tmp = ll_head_unusedBlock;
            ll_head_unusedBlock = ll_head_unusedBlock->next;
            free(tmp);

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

    DEBUG_PRINT(("[DEBUG] get_next_pca end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    return curr_pca.pca;
}

static int cache_read(char *data_buf, int write_buffer_lba)
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] cache_read start\n"));

    // TODO
    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] cache_read end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    return 0;
}

static int cache_write(const char *data, size_t logic_lba)
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] cache_write start\n"));

    // TODO
    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] cache_write end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    return 0;
}

static int ftl_flush()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_flush start\n"));

    // TODO
    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] ftl_flush end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    return 0;
}

static int ftl_read(char *buf, size_t lba)
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_read start\n"));

    // get pca from L2P table
    unsigned int pca = L2P[lba];
    DEBUG_PRINT(("[DEBUG] ftl_read from LBA %ld -> PCA %u\n", lba, pca));

    // return 0 if that slot did not have any data there
    if (pca == INVALID_PCA)
        return 0;

    // allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    // and call nand_read and store value inside the buffers (spare_bug & buf)
    int rst = nand_read(buf, spare_buf, pca);

    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] ftl_read end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));

    return rst;
}

static int ftl_write(const char *buf, size_t lba_range, size_t lba)
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_write start\n"));

    // get pca via calling get_next_pca()
    unsigned int pca = get_next_pca();
    DEBUG_PRINT(("[DEBUG] ftl_write from LBA %ld -> PCA %u\n", lba, pca));

    // allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(spare_buf, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE * sizeof(char));
    unsigned char state = 1;
    sprintf(spare_buf, "%c%c%c", state, lba / 256, lba % 256);

    // call nand_write to write the buffer to the NAND
    int rst = nand_write(buf, spare_buf, pca);
    if (rst == -EINVAL)
        return -EINVAL;

    /*** update linked list ***/
    /*** update table ***/
    // if nand_write complete successfully, then update the L2P table
    unsigned int pre_pca = L2P[lba];
    if (pre_pca != INVALID_PCA)
    {
        // Noted that when the slot was not empty, we need to update the IVC (collect there has a invalid page in that block)
        PCA_RULE my_pca;
        my_pca.pca = pre_pca;
        IVC[my_pca.fields.block] += 1;
    }
    L2P[lba] = pca;
    if (erasedSlot[lba] != 0)
        flag_updateLog = 1;
    erasedSlot[lba] = 0;

    /*** update log ***/

    DEBUG_PRINT(("[DEBUG] ftl_write end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    return rst;
}

static int ftl_gc()
{
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_gc start\n"));

    /*** check whether need to do gc ***/
    int num_unusedBlock = get_num_of_linkedList(ll_head_unusedBlock);
    DEBUG_PRINT(("[DEBUG] num_unusedBlock is %d\n", num_unusedBlock));
    print_linkedList(ll_head_unusedBlock);
    if (num_unusedBlock > 5)
    {
        DEBUG_PRINT(("[DEBUG] ftl_gc end\n"));
        DEBUG_PRINT(("-----------------------------------------------------\n"));
        return 0;
    }

    /*** decide the source block to be erased ***/
    int idx = 0;
    int idx_badBlock = -1, num_HighestIVC = 0;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        if (IVC[idx] > num_HighestIVC)
        {
            num_HighestIVC = IVC[idx];
            idx_badBlock = idx;
        }
    }
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
    rst = nand_erase(idx_badBlock);
    if (rst == -EINVAL)
        return -EINVAL;
    DEBUG_PRINT(("[DEBUG] ftl_gc, erase block success\n"));

    DEBUG_PRINT(("[DEBUG] ftl_gc end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
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
    tmp_buf = calloc(tmp_lba_range * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        // use ftl_read to read data and error handling
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

    // TODO
    /*** update linked list ***/
    /*** update table ***/
    /*** update log ***/

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

        int rst = ftl_read(tmp_buf, tmp_lba + idx);
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

        rst = ftl_write(tmp_buf, curr_size, tmp_lba + idx);
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
    DEBUG_PRINT(("-----------------------------------------------------\n"));
    DEBUG_PRINT(("[DEBUG] ftl_erase start\n"));

    // modify the lba mapping value in L2P table into INVALID_PCA, and update IVC table
    if (buf != NULL)
    {
        DEBUG_PRINT(("[DEBUG] ftl_erase, write the modified buffer to nand\n"));
        // ftl_write will already update IVC, don't collect two times
        int rst = ftl_write(buf, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, lba);
        if (rst == -EINVAL)
            return -EINVAL;
    }
    else
    {
        // if erase is aligned, just erase from L2P is enough, and update the log
        DEBUG_PRINT(("[DEBUG] ftl_erase, set the erasedSlot %d to 1\n", lba));

        /*** update linked list ***/
        /*** update table ***/
        unsigned int pca = L2P[lba];
        L2P[lba] = INVALID_PCA;
        PCA_RULE my_pca;
        my_pca.pca = pca;
        IVC[my_pca.fields.block] += 1;
        if (erasedSlot[lba] != 1)
            flag_updateLog = 1;
        erasedSlot[lba] = 1;

        /*** update log ***/
    }

    DEBUG_PRINT(("[DEBUG] ftl_erase end\n"));
    DEBUG_PRINT(("-----------------------------------------------------\n"));
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
        curr_offset = 0;
        if (idx == 0)
        {
            curr_offset = offset - (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
            curr_size = (offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE + 1) * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - offset;
            curr_size = curr_size > remain_size ? remain_size : curr_size;

            if (L2P[lba] == INVALID_PCA)
            {
                process_size += curr_size;
                remain_size -= curr_size;
                curr_size = 0;
                continue;
            }

            tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
            rst = ftl_read(tmp_buf, lba);
            if (rst == -EINVAL)
                return -EINVAL;
            memset(tmp_buf + curr_offset, 0, curr_size);
        }
        else if (idx == tmp_lba_range - 1)
        {
            curr_size = (remain_size);

            if (L2P[lba] == INVALID_PCA)
            {
                process_size += curr_size;
                remain_size -= curr_size;
                curr_size = 0;
                continue;
            }

            tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
            rst = ftl_read(tmp_buf, lba);
            if (rst == -EINVAL)
                return -EINVAL;
            memset(tmp_buf, 0, curr_size);
        }
        else
        {
            curr_size = PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
            if (L2P[lba] == INVALID_PCA)
            {
                process_size += curr_size;
                remain_size -= curr_size;
                curr_size = 0;
                continue;
            }
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

    /*** Restore from log ***/
    unusedBlock = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    // memset(unusedBlock, 0, sizeof(int) * PHYSICAL_NAND_NUM);
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        unusedBlock[idx] = 1;
    int is_exist_log = ftl_restore();

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
        DEBUG_PRINT(("[DEBUG] main, build ll_head_unusedBlock with block %d\n", idx));
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
