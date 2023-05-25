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

enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

// -------------------------------------------------------------- //
// FTL related data structure
// should restore all the data structure declared here after restart

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

unsigned int *L2P; // logical to physical table
unsigned int *IVC; // invalid count of a block
unsigned int *ERC; // erase count of a block

//
int *erasedSlot;

// A linked list node that points to next clean block
typedef struct node Node;
struct node
{
    int block;
    struct node *next;
};
Node *ll_nextBlock = NULL; // the head of the linked list
Node *ll_lastBlock = NULL; // the tail of the linked list
int num_remainBlock = 0;

Node *ll_blockOrder = NULL; // the order in which block is written

// -------------------------------------------------------------- //
// FTL-recover related helper data structure (no need to log/ spare them)

// a array to help re-consturct the linked list of clean blocks
unsigned int *block_in_ll;

// -------------------------------------------------------------- //

static int ssd_resize(size_t new_size)
{
    // set logic size to new_size
    if (new_size > LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024)
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
    FILE *fileRsult;
    size_t numwritten;
    fileRsult = fopen(LOG_LOCATION, "w"); // Use absoluate path to avoid permission denied
    if (fileRsult == NULL)
    {
        printf("fopen() failed with following error, %s\n", strerror(errno));
    }
    numwritten = fwrite(logBuf, sizeof(char), size, fileRsult);
    printf("write %zu bytes\n", numwritten);
    fclose(fileRsult);
    nand_write_size += 512;
}

size_t nand_read_log(unsigned char *logBuf, int size) // logBuf is output
{
    if (size > 512)
        return;
    FILE *fileRsult;
    size_t numwritten;

    fileRsult = fopen(LOG_LOCATION, "r");
    numwritten = fread(logBuf, sizeof(unsigned char), size, fileRsult);
    printf("read %zu bytes\n", numwritten);
    fclose(fileRsult);
    return numwritten;
}

void ftl_write_log()
{
    // We should write log in the following cituation
    // 1. when we call erase function
    // 2. when we can get_next_pca(), and we forward to a new block

    unsigned char *tmp_buf = malloc(512 * sizeof(unsigned char));
    memset(tmp_buf, 255, sizeof(unsigned char) * 512);
    int log_size = 0;

    /*** Store the block order into log ***/
    Node *curr_node = ll_blockOrder;
    int idx = 0, i = 0;
    while (curr_node)
    {
        char c = curr_node->block;
        // // BUG Even though curr_node->block is 1, the program still write 0 into log
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
    sprintf(tmp_buf + log_size, "%c", physic_size / 256);
    sprintf(tmp_buf + log_size + 1, "%c", physic_size % 256);
    log_size += 2;
    sprintf(tmp_buf + log_size, "%c", logic_size / 256);
    sprintf(tmp_buf + log_size + 1, "%c", logic_size % 256);
    log_size += 2;
    sprintf(tmp_buf + log_size, "%c", host_write_size / 256);
    sprintf(tmp_buf + log_size + 1, "%c", host_write_size % 256);
    log_size += 2;
    sprintf(tmp_buf + log_size, "%c", nand_write_size / 256);
    sprintf(tmp_buf + log_size + 1, "%c", nand_write_size % 256);
    log_size += 2;
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

    /*** Write log ***/
    // call nand_write_log() to write the log
    DEBUG_PRINT(("[DEBUG] ftl_write_log, stage 4  call nand_write_log\n"));
    nand_write_log(tmp_buf, 512);
    free(tmp_buf);
}

size_t spare_read(unsigned int pca);
int ftl_restore()
{
    DEBUG_PRINT(("=====================================================\n"));

    /*** Read log ***/
    // call nand_read_log() to read the log
    unsigned char *log_buf = calloc(512, sizeof(unsigned char));
    size_t log_size = nand_read_log(log_buf, 512);
    if (log_size == 0)
    {
        DEBUG_PRINT(("[DEBUG] ftl_restore, no log ...\n"));
        return 0;
    }

    /*** Restore IVC, L2P ***/
    // First, we should restore ll_blockOrder, we will then use this order to recover L2P, IVC table
    // log_buf[0: 51] stored the value the block order
    int idx = 0, i = 0;
    Node *prev_node = (Node *)malloc(sizeof(Node)); // Initialze a dummy node first
    prev_node->next = NULL;
    prev_node->block = 50;
    Node *dummy_head = prev_node;
    for (idx = 0; idx < 50; idx++)
    {
        int block = log_buf[idx];
        if (block == 255)
            break;
        DEBUG_PRINT(("[DEBUG] ftl_restore, restore ll_blockOrder %d-th, with value %d\n", idx, block));

        Node *new_node = (Node *)malloc(sizeof(Node));
        new_node->block = block;
        new_node->next = NULL;

        prev_node->next = new_node;
        prev_node = prev_node->next;
    }
    ll_blockOrder = dummy_head->next;
    free(dummy_head);

    // New, we read the spare data from nand, and recover it to L2P in the order described by ll_blockOrder
    //  at the same time, we can recover IVC table by checking which L2P slot is overwrited
    Node *curr_node = ll_blockOrder;
    int *prev_block = (int *)malloc(sizeof(int) * LBA_NUM);
    memset(prev_block, -1, sizeof(int) * LBA_NUM);

    int is_lastBlock = 0;
    while (curr_node)
    {
        // read the spare data of all pages at current block
        int page = 0;
        for (page = 0; page < NAND_SIZE_KB * 1024 / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE; page++)
        {
            // Read from the spare data from the page of the block to get lba
            PCA_RULE my_pca;
            my_pca.fields.block = curr_node->block;
            my_pca.fields.page = page;
            size_t lba = spare_read(my_pca.pca);

            // Check the last inserted block, if it not full, then we should restore the curr_pca to the last page on it
            if (curr_node->next == NULL && lba == LBA_NUM)
            {
                curr_pca.fields.block = curr_node->block;
                curr_pca.fields.page = page;
                break;
            }

            // if lba is already occupied, it means that there exist a invalid slot in the previous block
            if (L2P[lba] != INVALID_PCA)
            {
                IVC[prev_block[lba]] += 1;
            }
            prev_block[lba] = curr_node->block;

            // update the pca on L2P table
            L2P[lba] = my_pca.pca;
        }

        //
        block_in_ll[curr_node->block] = 1;
        DEBUG_PRINT(("[DEBUG] ftl_restore, set block_in_ll[%d] to 1\n", curr_node->block));
        curr_node = curr_node->next;
    }

    /*** Restore ERC ***/
    // log_buf[50: 101] stored the value the ERC table
    for (idx = 0; idx < 50; idx++)
    {
        ERC[idx] = log_buf[50 + idx];
        DEBUG_PRINT(("[DEBUG] ftl_restore, restore ERC table %d-th, with value %d\n", idx, ERC[idx]));
    }

    /*** Restore WAF ***/
    // Restore physical size, host_size, etc.
    for (idx = 0; idx < 2 * 4; idx += 2)
    {
        //
        size_t size = 0;
        size = log_buf[100 + idx] * 256 + log_buf[100 + idx + 1];
        DEBUG_PRINT(("[DEBUG] ftl_restore, restore WAF table %d-th, with value %u\n", idx / 2, size));
        if (idx == 0)
            physic_size = size;
        else if (idx / 2 == 1)
            logic_size = size;
        else if (idx / 2 == 2)
            host_write_size = size;
        else if (idx / 2 == 3)
            nand_write_size = size;
    }

    /*** Restore erasedSlot, and L2P ***/
    int value = -1, slot = -1;
    for (idx = 0; idx < 100; idx++)
    {
        value = log_buf[108 + idx];
        for (i = 0; i < 8; i++)
        {
            slot = (idx + 1) * 8 - i - 1;
            erasedSlot[slot] = value % 2;
            if (erasedSlot[slot] == 1)
            {
                DEBUG_PRINT(("[DEBUG] ftl_restore, restore erasedSlot table %d-th, with value %d\n", slot, erasedSlot[slot]));

                // erasedSlot is 1, set the L2P[slot] to INVALID_PCA, and IVC[slot] increase by 1
                PCA_RULE my_pca;
                my_pca.pca = L2P[slot];
                IVC[my_pca.fields.block] += 1;
                L2P[slot] = INVALID_PCA;
            }
            value = value / 2;
        }
    }

    DEBUG_PRINT(("=====================================================\n"));

    return 1;
}

static int nand_read(char *data_buf, char *spare_buf, int pca)
{
    char nand_name[100];
    FILE *fptr;
    PCA_RULE my_pca;
    my_pca.pca = pca; // the value inside pca (32) will divide into and sent to fields.page (16) and block (16)
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

    /*** FTL maintain ***/
    // remove the block from ll_blockOrder
    Node *curr_node = ll_blockOrder;
    Node *prev_node = NULL;
    while (curr_node)
    {
        if (curr_node->block == nand)
        {
            if (prev_node != NULL)
            {
                prev_node->next = prev_node->next->next;
                free(curr_node);
            }
            else
            {
                prev_node = curr_node;
                ll_blockOrder = ll_blockOrder->next;
                free(prev_node);
            }
            break;
        }
        prev_node = curr_node;
        curr_node = curr_node->next;
    }

    // update the ERC table
    ERC[nand]++;

    // update the log
    ftl_write_log();

    printf("nand erase %d pass\n", nand);
    return 1;
}

static int ftl_gc();
static unsigned int get_next_pca()
{
    // Modify writing sequence A to sequence B
    // [x] Change next pca address to next page instead of next NAND

    if (curr_pca.pca == INVALID_PCA)
    {
        /*** Forward the curr_pca to the new block that pointed by ll_nextBlock ***/
        // initialization
        // After starup, the curr_pca is always INVALID_PCA, initial curr_pca to the clean block
        curr_pca.fields.block = ll_nextBlock->block;
        curr_pca.fields.page = 0;
        Node *curr_node = ll_nextBlock;
        ll_nextBlock = ll_nextBlock->next;
        free(curr_node);

        /*** Update the block write order thaa should stored in the log later ***/
        // And we have the first block being written
        Node *new_node = (Node *)malloc(sizeof(Node));
        new_node->block = curr_pca.fields.block;
        new_node->next = NULL;
        ll_blockOrder = new_node;

        // write log whenever we forward to a new block
        ftl_write_log();

        printf("PCA = lba %d, nand %d, init\n", curr_pca.fields.page, curr_pca.fields.block);
        DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d, init\n", curr_pca.fields.block));
        return curr_pca.pca;
    }
    else if (curr_pca.pca == FULL_PCA)
    {
        // full ssd, no pca can allocate
        printf("No new PCA\n");
        return FULL_PCA;
    }

    // When current block did not meet the last page, just forward to the next page on the same block
    // else, forward to next block and start from page 0
    if (curr_pca.fields.page >= LBA_NUM_PER_BLOCK - 1)
    {
        // if the current block is already the last block, then there is no new place for new PCA
        // After doing garbage collection, curr_pca should forward to the clean block, insteaf of next block -> linked list instead of round-robin
        if (ll_nextBlock == NULL)
        {
            printf("No new PCA\n");
            curr_pca.pca = FULL_PCA;
            return FULL_PCA;
        }
        else
        {
            /*** Forward the curr_pca to the new block that pointed by ll_nextBlock ***/
            // We still have clean blocks, then move to the block pointed by ll_nextBlock
            curr_pca.fields.block = ll_nextBlock->block;
            Node *curr_node = ll_nextBlock;
            ll_nextBlock = ll_nextBlock->next;
            free(curr_node);
            num_remainBlock--;

            curr_pca.fields.page = 0;
            printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
            DEBUG_PRINT(("[DEBUG] get_next_pca, forward to a new block %d\n", curr_pca.fields.block));

            /*** Update the block write order thaa should stored in the log later ***/
            // First, create a new node store the number of the block being written currently
            Node *new_node = (Node *)malloc(sizeof(Node));
            new_node->block = curr_pca.fields.block;
            new_node->next = NULL;

            // Before insert the new block, we have to delete the duplicate node (if there exists)
            curr_node = ll_blockOrder;
            Node *prev_node = (Node *)malloc(sizeof(Node));
            prev_node->block = 50;
            prev_node->next = curr_node;
            Node *dummy_head = prev_node;
            int isFind = 0;
            while (curr_node)
            {
                if (curr_node->block == curr_pca.fields.block)
                {
                    isFind = 1;
                    break;
                }
                prev_node = curr_node;
                curr_node = curr_node->next;
            }
            if (isFind)
            {
                Node *temp = prev_node->next;
                prev_node->next = prev_node->next->next;
                free(temp);
            }

            free(curr_node);
            free(dummy_head);
            DEBUG_PRINT(("[DEBUG] get_next_pca, delete the previous duplicate node\n"));

            // Now, we can insert the block to the tail of the block order linked list
            curr_node = ll_blockOrder;
            while (curr_node->next)
            {
                curr_node = curr_node->next;
            }
            curr_node->next = new_node;
            DEBUG_PRINT(("[DEBUG] get_next_pca, insert the new order into ll_blockOrder\n"));

            // At last, we should update the new block order on log
            ftl_write_log();

            return curr_pca.pca;
        }
    }
    else
    {
        curr_pca.fields.page += 1;
        printf("PCA = lba %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
        return curr_pca.pca;
    }
}

static int ftl_read(const char *buf, size_t lba)
{
    // [x] Use LBA and L2P table to find PCA
    // [x] Read data to buffer with given PCA

    // Get pca from L2P table
    unsigned int pca = L2P[lba];
    DEBUG_PRINT(("[DEBUG] ftl_read from LBA %ld -> PCA %u\n", lba, pca));

    // return 0 if that slot did not have any data there
    if (pca == INVALID_PCA)
    {
        return 0;
    }

    // Allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    // Call nand_read and store value inside the buffers (spare_bug & buf)
    int rst = nand_read(buf, spare_buf, pca);
    return rst;
}

size_t spare_read(unsigned int pca)
{
    /*** Giving PCA, return LBA stored in its spare data ***/

    DEBUG_PRINT(("[DEBUG] spare_read(pca) from pca %u \n", pca));

    // return 0 if that slot did not have any data there
    if (pca == INVALID_PCA)
    {
        return 0;
    }

    // Allocate space for spare buffer
    char *buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    unsigned char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));

    // Call nand_read and store value inside the buffers (spare_buf)
    int rst = nand_read(buf, spare_buf, pca);
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
    DEBUG_PRINT(("[DEBUG] spare_read(pca) get lba =  %ld \n", lba));

    return lba;
}

static int ftl_write(const char *buf, size_t lba_rnage, size_t lba)
{
    // [x] Use get_next_pca to find empty PCA for data writing
    // [x] Write data from buffer to PCA
    // [x] Update L2P table

    // Remember that NAND can not overwrite, even though we can 'overwrite' it via changing the mapping value in L2P,
    // we still have to record some metric first to help garbage collection

    // Get pca via calling get_next_pca()
    unsigned int pca = get_next_pca();
    DEBUG_PRINT(("[DEBUG] ftl_write from LBA %ld -> PCA %u, remainBlock %d\n", lba, pca, num_remainBlock));
    // Allocate space for spare buffer
    char *spare_buf = calloc(PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE, sizeof(char));
    memset(spare_buf, 0, PHYSICAL_SPARE_SIZE_BYTES_PER_PAGE * sizeof(char));
    sprintf(spare_buf, "%c%c%c", 1, lba / 256, lba % 256);

    DEBUG_PRINT(("[DEBUG] write lba %ld to %u  \n", lba, pca));
    // Call nand_write to write the buffer to the NAND
    int rst = nand_write(buf, spare_buf, pca);
    if (rst == -EINVAL)
        return -EINVAL;

    // If nand_write complete successfully, then update the L2P table
    unsigned int pre_pca = L2P[lba];
    if (pre_pca != INVALID_PCA)
    {
        // Noted that when the slot was not empty, we need to update the IVC (collect there has a invalid page in that block)
        PCA_RULE my_pca;
        my_pca.pca = pre_pca;
        IVC[my_pca.fields.block] += 1;
    }
    L2P[lba] = pca;
    erasedSlot[lba] = 0;
    return rst;
}

static int ftl_gc()
{
    // [x] Decide the source block to be erased
    // [x] Move all the valid data that in source block to another block
    // [x] Update L2P table
    // [x] Erase the source block with invalid data

    // Before empty blocks runs out, we should do garbage collections
    // Currently, we do garbage collection whenever the number of clean blocks is lower then some threshold (declared in ssd_do_write)

    // Decide the source block to be erased via comparing the IVC, the larger one has more priority
    // Add ERC value into consideration
    int idx = 0;
    int idx_highestIVC_block = -1, num_highestIVC_block = 0;
    for (int idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        if (IVC[idx] > num_highestIVC_block)
        {
            num_highestIVC_block = IVC[idx];
            idx_highestIVC_block = idx;
        }
    }
    DEBUG_PRINT(("[DEBUG] Select %d as source block to erase, which IVC is %d\n", idx_highestIVC_block, num_highestIVC_block));

    // classify valid/invalid data in the block
    unsigned int is_valid_data[LBA_NUM_PER_BLOCK];
    memset(is_valid_data, INVALID_PCA, sizeof(int) * LBA_NUM_PER_BLOCK);
    int num_valid_data = 0;
    for (idx = 0; idx < LBA_NUM; idx++)
    {
        // if there is a slot in L2P that points to this block,
        //      collect the LBA into the is_valid_data
        unsigned int pca = L2P[idx];
        PCA_RULE my_pca;
        my_pca.pca = pca;
        if (my_pca.fields.block == idx_highestIVC_block)
        {
            is_valid_data[my_pca.fields.page] = idx;
            num_valid_data++;
            DEBUG_PRINT(("[DEBUG] is_valid_data[%u] = %d\n", my_pca.fields.page, idx));
        }
    }
    DEBUG_PRINT(("[DEBUG] Number of valid data in block: %d\n", num_valid_data));

    // Move all the valid data into another block
    int rst;
    for (idx = 0; idx < LBA_NUM_PER_BLOCK; idx++)
    {
        //
        if (is_valid_data[idx] == INVALID_PCA)
            continue;

        //
        int tmp_lba = is_valid_data[idx];
        char *tmp_buf = malloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE * sizeof(char));
        rst = ftl_read(tmp_buf, tmp_lba);
        if (rst == -EINVAL)
            return -EINVAL;

        //
        rst = ftl_write(tmp_buf, 1, tmp_lba);
        if (rst == -EINVAL)
            return -EINVAL;
    }
    DEBUG_PRINT(("[DEBUG] Move all the valid data into another block\n"));

    // Erase the source block
    rst = nand_erase(idx_highestIVC_block);
    if (rst == -EINVAL)
        return -EINVAL;
    DEBUG_PRINT(("[DEBUG] Erase the source block: %d\n", idx_highestIVC_block));

    // Update the related data structure, including IVC, linked list for clean block, num_remainBlock
    IVC[idx_highestIVC_block] = 0;
    num_remainBlock++;

    Node *curr_block = (Node *)malloc(sizeof(Node));
    curr_block->block = idx_highestIVC_block;
    curr_block->next = NULL;
    ll_lastBlock->next = curr_block;
    ll_lastBlock = ll_lastBlock->next;
    DEBUG_PRINT(("[DEBUG] Successfully erase block %d\n", idx_highestIVC_block));

    return rst;
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
    // [x] Check the location of read command valid or invalid
    // [x] Divide read cmd into 512B package by size
    // [x] Use ftl_read to read data and error handling

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

    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    if ((offset + size) % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0)
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    else
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba);

    tmp_buf = calloc(tmp_lba_range * PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
    DEBUG_PRINT(("[DEBUG] total logic_size %ld, ssd_do_read with size %ld, offset %ld ==> LBA from %d -> %d\n", logic_size, size, offset, tmp_lba, tmp_lba_range + tmp_lba - 1));

    for (int i = 0; i < tmp_lba_range; i++)
    {
        // call ftl_read() with logical adress one by one
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
    DEBUG_PRINT(("[DEBUG] ssd_read with size %ld, offset %ld\n", size, offset));
    (void)fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

static int ssd_do_write(const char *buf, size_t size, off_t offset)
{
    // [x] Divide write cmd into 512B package by size
    // [x] Use ftl_write to write data
    // [x] Need to handle writing non-aligned data

    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, curr_offset;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // Check if the write-in data is aligned or not
    // offset is alignd whenever it is divisble by 512
    int isAlign = 1;
    if (offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0)
    {
        isAlign = 0;
    }

    //
    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    if ((offset + size) % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0)
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    else
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba);
    DEBUG_PRINT(("[DEBUG] ssd_do_write with size %d, offset %d ==> LBA from %d -> %d\n", size, offset, tmp_lba, tmp_lba_range + tmp_lba - 1));

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    curr_offset = 0;
    int is_overWriteErasedSlot = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {

        // TODO: Modify the time stamp to do garbage collection
        if (num_remainBlock <= (PHYSICAL_NAND_NUM - LOGICAL_NAND_NUM) / 2)
        {
            DEBUG_PRINT(("[DEBUG] Begin gc, remainBlock %d is ver low\n", num_remainBlock));
            ftl_gc();
            DEBUG_PRINT(("[DEBUG] After gc, remainBlock is %d\n", num_remainBlock));
        }

        // Divide write-in value into 512B package by size, processing them sequencially
        // tmp_buf is the current 512B write-in value
        char *tmp_buf = calloc(PHYSICAL_DATA_SIZE_BYTES_PER_PAGE, sizeof(char));
        memset(tmp_buf, 0, PHYSICAL_DATA_SIZE_BYTES_PER_PAGE);

        //
        int rst = ftl_read(tmp_buf, tmp_lba + idx);
        if (rst == -EINVAL)
        {
            return -EINVAL;
        }

        // Handling non-aligned data
        curr_offset = 0;
        // If we met a non-aliged write-in date, then the first and the last 512B data need to be handled differently
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

        // If we met the first part of non-aliged data, we should move the buffer toward right
        DEBUG_PRINT(("[DEBUG] ssd_do_write fill from %d to %d\n", curr_offset, curr_offset + curr_size));
        memcpy(tmp_buf + curr_offset, buf + process_size, curr_size);

        // call ftl_write() with logical adress one by one
        rst = ftl_write(tmp_buf, curr_size, tmp_lba + idx);
        if (rst == -EINVAL)
        {
            return -EINVAL;
        }
        free(tmp_buf);

        //
        if (erasedSlot[tmp_lba + idx] == 1)
        {
            is_overWriteErasedSlot = 1;
            erasedSlot[tmp_lba + idx] = 0;
        }

        // Update the process_, remain_, curr_ size
        process_size += curr_size;
        remain_size -= curr_size;
    }

    if (is_overWriteErasedSlot == 1)
    {
        ftl_write_log();
    }

    return size;
}

static int ssd_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

    (void)fi;
    DEBUG_PRINT(("=====================================================\n"));
    DEBUG_PRINT(("[DEBUG] ssd_write with size %ld, offset %ld\n", size, offset));
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

static int ssd_do_erase(int offset, int size)
{
    // [x] Divide erase cmd into 512B package by size
    // [x] Need to handle non-aligned erase dat

    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size;
    int is_log = 0;
    int curr_offset = 0;

    //
    int isAlign = 1;
    if (offset % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0)
    {
        isAlign = 0;
    }

    //
    tmp_lba = offset / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE;
    if ((offset + size) % PHYSICAL_DATA_SIZE_BYTES_PER_PAGE != 0)
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba) + 1;
    else
        tmp_lba_range = (offset + size) / PHYSICAL_DATA_SIZE_BYTES_PER_PAGE - (tmp_lba);

    DEBUG_PRINT(("[DEBUG] ssd_do_erase with size %d, offset %d ==> LBA from %d -> %d\n", size, offset, tmp_lba, tmp_lba_range + tmp_lba - 1));

    // BUG
    process_size = 0;
    remain_size = size;
    curr_size = 0;
    curr_offset = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        //
        size_t lba = tmp_lba + idx;
        // erasedSlot[lba] = 1;

        // Handling non-aligned data
        // For non-alignmented issue, read the whole page, erase the data, and move the remain data to a new page
        const char *tmp_buf = NULL;
        curr_offset = 0;
        // the same, we have to take the first and last 512B data into consideration
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
            int rst = ftl_read(tmp_buf, lba);
            if (rst == -EINVAL)
            {
                return -EINVAL;
            }
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
            int rst = ftl_read(tmp_buf, lba);
            if (rst == -EINVAL)
            {
                return -EINVAL;
            }
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

        // Modify the lba mapping value in L2P table into INVALID_PCA, and update IVC table
        if (tmp_buf != NULL)
        {
            // ftl_write will already update IVC, don't collect two times
            int rst = ftl_write(tmp_buf, curr_size, lba);
            if (rst == -EINVAL)
            {
                return -EINVAL;
            }
        }
        else
        {
            // If erase is aligned, just erase from L2P is enough, and update the log
            unsigned int pca = L2P[lba];
            L2P[lba] = INVALID_PCA;
            PCA_RULE my_pca;
            my_pca.pca = pca;
            IVC[my_pca.fields.block] += 1;
            is_log = 1;
            erasedSlot[lba] = 1;
        }

        // Update the process_, remain_, curr_ size
        process_size += curr_size;
        remain_size -= curr_size;
        curr_size = 0;
    }

    // update the log
    if (is_log == 1)
        ftl_write_log();
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
        unsigned long long eraseFrame = *(unsigned long long *)data;
        int eraseSize = eraseFrame & 0xFFFFFFFF;
        int eraseStart = (eraseFrame >> 32) & 0xFFFFFFFF;
        printf(" --> erase start: %u, erase size: %u\n", eraseStart, eraseSize);
        ssd_do_erase(eraseStart, eraseSize);
    }
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

int main(int argc, char *argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
    nand_write_size = 0;
    host_write_size = 0;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LBA_NUM * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int) * LBA_NUM);

    // Initialize for IVC with all 0s (at first, the invalid count is always 0)
    IVC = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(IVC, 0, sizeof(int) * PHYSICAL_NAND_NUM);

    // Initialize for ERC with all 0s (at first, the invalid count is always 0)
    ERC = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(ERC, 0, sizeof(int) * PHYSICAL_NAND_NUM);

    // Initialize for erasedSlot with all 0s (at first, the slot is not erased)
    erasedSlot = malloc(LBA_NUM * sizeof(int));
    memset(erasedSlot, 0, sizeof(int) * LBA_NUM);

    DEBUG_PRINT(("[DEBUG] main, intialize memory for all array complete\n"));

    /*** Restore from the log ***/
    block_in_ll = malloc(sizeof(int) * PHYSICAL_NAND_NUM);
    memset(block_in_ll, 0, sizeof(int) * PHYSICAL_NAND_NUM); // BUG Do not memset with init value 1
    ll_blockOrder = NULL;

    int rst = ftl_restore();
    DEBUG_PRINT(("[DEBUG] main, ftl_restore complete\n"));

    /*** Construct the linked list for clean blocks ***/
    // Initialize a dummy node at first
    ll_nextBlock = (Node *)malloc(sizeof(Node));
    ll_nextBlock->block = 50;
    ll_nextBlock->next = NULL;
    ll_lastBlock = ll_nextBlock;
    num_remainBlock = 0;
    Node *new_node = NULL;
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        //
        if (block_in_ll[idx] == 1)
            continue;
        DEBUG_PRINT(("[DEBUG] main, build linked list with block %d\n", idx));

        //
        new_node = (Node *)malloc(sizeof(Node));
        new_node->block = idx;
        new_node->next = NULL;
        num_remainBlock++;

        // concat the new node to the linked list
        ll_lastBlock->next = new_node;
        ll_lastBlock = ll_lastBlock->next;
    }
    Node *curr_node = ll_nextBlock;
    ll_nextBlock = ll_nextBlock->next;
    // TODO Why I can not free the following two pointer ?  -> free() invalid pointer
    // https://blog.csdn.net/qq_24345071/article/details/118713298
    // printf("curr_node: %p\n", curr_node);
    free(curr_node);
    free(block_in_ll);
    DEBUG_PRINT(("[DEBUG] main, construct the linked list for clean blocks complete\n"));

    /*** Create nand block ***/
    // If there did not exist any log before (rst == 0), then create nand file (or refresh them to clean state)
    if (rst == 0)
    {
        for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
        {
            FILE *fptr;
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
