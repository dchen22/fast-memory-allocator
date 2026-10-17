#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>

/* fl = floor(log2(size)), so 1MiB (2^20) needs at least 21 rows. */
#define FL_SIZE 32
#define SL_SIZE 4
#define POOL_SIZE (1 << 20)

typedef struct BlockHeader {
    uint32_t size;
    struct BlockHeader* prev;
    struct BlockHeader* next;
} BlockHeader;

void getLayerIndicesFromBlockSize(
    uint32_t size, uint32_t* firstLayerIndex, uint32_t* secondLayerIndex);
uint64_t align(uint64_t x);
void insert_free_block(
    BlockHeader* free_grid[FL_SIZE][SL_SIZE], BlockHeader* block);
BlockHeader* findLargeEnoughFreeBlock(
    BlockHeader* free_grid[FL_SIZE][SL_SIZE],
    uint32_t size, uint32_t* found_fl, uint32_t* found_sl);
void* tlsf_malloc(BlockHeader* free_grid[FL_SIZE][SL_SIZE], uint32_t n);

int main()
{
    int err = 0;
    BlockHeader* free_grid[FL_SIZE][SL_SIZE];
    void* pool = mmap(NULL, POOL_SIZE, 
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    
    if (MAP_FAILED == pool)
    {
        err = -1;
    }

    if (!err)
    {
        uint32_t initialFlIndex = 0, initialSlIndex = 0;

        // zero init free grid
        memset(free_grid, 0, sizeof(free_grid));

        // Get grid index of entire pool as an initial block
        getLayerIndicesFromBlockSize(
            POOL_SIZE, &initialFlIndex, &initialSlIndex);

        // Create header for pool block
        BlockHeader initialPoolBlock = {
            .size = POOL_SIZE,
            .next = NULL,
            .prev = NULL,
        };
        
        // Write header to start of pool block
        memcpy(pool, &initialPoolBlock, sizeof(BlockHeader));
        
        // Add pool block to free grid
        free_grid[initialFlIndex][initialSlIndex] = (BlockHeader*)pool;

        if (NULL == tlsf_malloc(free_grid, 50))
        {
            err = -1;
        }
    }

    return err;
}

void getLayerIndicesFromBlockSize(
    uint32_t size, uint32_t* firstLayerIndex, uint32_t* secondLayerIndex)
{
    /* size 0 has no log; treat as the smallest bucket */
    if (0 == size)
    {
        *firstLayerIndex = 0;
        *secondLayerIndex = 0;
        return;
    }

    /* first layer: floor(log2(size)).
       size in [2^fl, 2^{fl+1}), e.g. 50 -> fl 5 -> row [32, 64). */
    uint32_t fl = 0;
    uint32_t sz = size;
    while (sz > 1)
    {
        sz >>= 1;
        fl++;
    }

    /* second layer: split [2^fl, 2^{fl+1}) into SL_SIZE equal bands.
       sl = how far we are into that range, scaled to 0 .. SL_SIZE-1. */
    uint32_t range_start = 1u << fl;
    uint32_t sl = (uint32_t)(((uint64_t)(size - range_start) * SL_SIZE) >> fl);

    *firstLayerIndex = fl;
    *secondLayerIndex = sl;
}

uint64_t align(uint64_t x)
{
    return (x + 15) & ~15;
}

void insert_free_block(
    BlockHeader* free_grid[FL_SIZE][SL_SIZE], BlockHeader* block)
{
    uint32_t fl = 0, sl = 0;
    getLayerIndicesFromBlockSize(block->size, &fl, &sl);
    block->prev = NULL;
    block->next = free_grid[fl][sl];
    if (NULL != free_grid[fl][sl])
    {
        free_grid[fl][sl]->prev = block;
    }
    free_grid[fl][sl] = block;
}

/* Return smallest size a block in free_grid[fl][sl] can have */
static uint32_t bandLowerBound(uint32_t fl, uint32_t sl)
{
    uint32_t range_start = 1u << fl;
    uint32_t band_width = range_start / SL_SIZE;
    return range_start + sl * band_width;
}

/* 
Find first free block of the first band with free blocks guaranteed to fit <size>.
Also get grid index of this band.

E.g. if size=50 and bands are 46-49, 48-51, 52-55,
the 48-51 band fits 50 but 52-55 blocks are guaranteed to fit 50.
Also must actually have free blocks in that band

Returns NULL if no such free block exists. 
*/
BlockHeader* findLargeEnoughFreeBlock(
    BlockHeader* free_grid[FL_SIZE][SL_SIZE],
    uint32_t size, uint32_t* found_fl, uint32_t* found_sl)
{
    uint32_t fl = 0, sl = 0;
    getLayerIndicesFromBlockSize(size, &fl, &sl);

    /* Insert mapping can land in a band whose lower bound is still < size
       (e.g. 50 sits in 48–55). Skip any such band; do not walk its list.
       Later this scan becomes a bitmap/ctz jump. */
    for (uint32_t i = fl; i < FL_SIZE; i++)
    {
        uint32_t j0 = (i == fl) ? sl : 0;
        for (uint32_t j = j0; j < SL_SIZE; j++)
        {
            if (NULL != free_grid[i][j] && bandLowerBound(i, j) >= size)
            {
                *found_fl = i;
                *found_sl = j;
                return free_grid[i][j];
            }
        }
    }
    return NULL;
}

void* tlsf_malloc(BlockHeader* free_grid[FL_SIZE][SL_SIZE], uint32_t n)
{
    /* User asked for n payload bytes. Block must also hold the header */
    uint32_t need = (uint32_t)align(n + sizeof(BlockHeader));

    /* First band whose every block is >= need */
    uint32_t found_fl = 0, found_sl = 0;
    /* Get first free block */
    BlockHeader* block = findLargeEnoughFreeBlock(
        free_grid, need, &found_fl, &found_sl);
    if (NULL == block)  
    {
        return NULL;
    }

    /* Pop this free block */
    free_grid[found_fl][found_sl] = block->next;
    if (NULL != block->next)
    {
        block->next->prev = NULL;
    }
    block->prev = NULL;
    block->next = NULL;

    /* Split: 
    We claim <need> bytes from the free block. Is the leftover large
    enough to file as a new free block? 

    If so, our claimed free block will only take <need> bytes, and file the leftover as a new block

    Otherwise, our claimed free block will keep its entire original size, with no leftover (leftover would be too small anyway) 
    */
    if (block->size - need >= sizeof(BlockHeader))
    {
        /* Start leftover past the <need> bytes we kept */
        BlockHeader* tail = (BlockHeader*)((char*)block + need);
        tail->size = block->size - need;
        tail->prev = NULL;
        tail->next = NULL;
        block->size = need;
        insert_free_block(free_grid, tail);
    }

    /* Caller gets the payload, which starts after the header */
    return (char*)block + sizeof(BlockHeader);
}
