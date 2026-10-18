#include <stdlib.h>
#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include "tlsf.h"

/* fl = floor(log2(size)), so 1MiB (2^20) needs at least 21 rows. */
#define FL_SIZE 32
#define SL_SIZE 4
#define POOL_SIZE (1 << 20)

typedef struct BlockHeader {
    uint32_t size;
    struct BlockHeader* prev;
    struct BlockHeader* next;
    bool isFree;
} BlockHeader;

typedef struct TlsfGlobal {
    char* pool;
    uint32_t poolSize;
    BlockHeader* free_grid[FL_SIZE][SL_SIZE];
    uint32_t firstLayerBitmap;
    uint32_t secondLayerBitmap[FL_SIZE];
} TlsfGlobal;

void getLayerIndicesFromBlockSize(
    uint32_t size, uint32_t* firstLayerIndex, uint32_t* secondLayerIndex);
uint64_t align(uint64_t x);
void insert_free_block(
     BlockHeader* block);
BlockHeader* findLargeEnoughFreeBlock(
    
    uint32_t size, uint32_t* found_fl, uint32_t* found_sl);
void popBlockHeaderNode( uint32_t fl, uint32_t sl, BlockHeader* node);
bool isHeaderInPoolRange(BlockHeader* pBlockHeader);

TlsfGlobal tlsf;

int tlsf_init(void)
{
    assert(POOL_SIZE > 2 * sizeof(BlockHeader));

    if (NULL != tlsf.pool && MAP_FAILED != (void *)tlsf.pool)
    {
        munmap(tlsf.pool, tlsf.poolSize);
    }
    memset(&tlsf, 0, sizeof(tlsf));

    void *pool = mmap(NULL, POOL_SIZE,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);

    if (MAP_FAILED == pool)
    {
        return -1;
    }

    tlsf.pool = pool;
    tlsf.poolSize = POOL_SIZE;
    memset(tlsf.free_grid, 0, sizeof(tlsf.free_grid));

    BlockHeader *pPoolHeader = pool;
    pPoolHeader->size = POOL_SIZE;
    pPoolHeader->next = NULL;
    pPoolHeader->prev = NULL;
    pPoolHeader->isFree = true;
    insert_free_block(pPoolHeader);
    return 0;
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

void insert_free_block(BlockHeader* block)
{
    uint32_t fl = 0, sl = 0;
    getLayerIndicesFromBlockSize(block->size, &fl, &sl);
    block->prev = NULL;
    block->next = tlsf.free_grid[fl][sl];
    if (NULL != tlsf.free_grid[fl][sl])
    {
        tlsf.free_grid[fl][sl]->prev = block;
    }
    tlsf.free_grid[fl][sl] = block;

    block->isFree = true;

    // update bitmaps
    tlsf.firstLayerBitmap |= 1u << fl;
    tlsf.secondLayerBitmap[fl] |= 1u << sl;

    BlockHeader* pFooter = (BlockHeader*)((char*)block + block->size - sizeof(BlockHeader));
    memcpy(pFooter, block, sizeof(BlockHeader));
}

/* Return smallest size a block in free_grid[fl][sl] can have */
static uint32_t bandLowerBound(uint32_t fl, uint32_t sl)
{
    uint32_t range_start = 1u << fl;
    uint32_t band_width = range_start / SL_SIZE;
    return range_start + sl * band_width;
}

/* 
Find first free block of the first safe band

A safe band is a band for block sizes [low, high) where 
<size> <= low. This means any block in that band can accomodate our
desired size. 

Also get the grid index of this band.

E.g. if size=50 and bands are 46-49, 48-51, 52-55,
the 48-51 band fits 50 but 52-55 blocks are guaranteed to fit 50.
Also must actually have free blocks in that band

Returns NULL if no such free block exists. 
*/
BlockHeader* findLargeEnoughFreeBlock(
    uint32_t size, uint32_t* found_fl, uint32_t* found_sl)
{
    uint32_t fl = 0, sl = 0;
    getLayerIndicesFromBlockSize(size, &fl, &sl);

    /* 
    Find first safe band (uses nested for loop but is at most 2
    iterations)
    */

    // If (fl, sl) is not a safe band, start at immediate next band
    if (bandLowerBound(fl, sl) < size)  
    {
        if (sl < SL_SIZE - 1)   // More bands in this FL row 
        {
            sl++;
        }
        else
        {
            if (fl < FL_SIZE - 1)   // More FL rows
            {
                fl++;
                sl = 0;
            }
            else    // Reached the biggest band and still can't fit
            {
                return NULL;
            }
        }
    }

    /* Check if the safe band's row has a free block, starting
    at the free band
    */
    for (uint32_t i = sl; i < SL_SIZE; i++)
    {
        if (NULL != tlsf.free_grid[fl][i])
        {
            *found_fl = fl;
            *found_sl = i;
            return tlsf.free_grid[fl][i];
        }
    }

    /* Safe band row doesn't have free block. 
    Do bitmap search to find next row with a free block */
    uint32_t flMask = (~((1u << fl) - 1)) << 1; // only bits after fl-th bit 
    uint32_t firstLayerBitmapMasked = tlsf.firstLayerBitmap & flMask;    
    if (0 == firstLayerBitmapMasked)    // Nothing free
    {
        return NULL;
    }
    uint32_t availableFl = __builtin_ctz(firstLayerBitmapMasked);   // First FL row with free block
    uint32_t availableSl = __builtin_ctz(tlsf.secondLayerBitmap[availableFl]);
    
    *found_fl = availableFl;
    *found_sl = availableSl;
    return tlsf.free_grid[availableFl][availableSl];
}

void* tlsf_malloc(uint32_t n)
{
    /* User asked for n payload bytes. Block must also hold the header and footer */
    uint32_t need = (uint32_t)align(n + 2*sizeof(BlockHeader));

    /* First band whose every block is >= need */
    uint32_t found_fl = 0, found_sl = 0;
    /* Get first free block */
    BlockHeader* pClaimedHeader = findLargeEnoughFreeBlock(
        need, &found_fl, &found_sl);
    BlockHeader* pClaimedFooter;
    if (NULL == pClaimedHeader)  
    {
        return NULL;
    }

    /* Pop this free block */
    popBlockHeaderNode(found_fl, found_sl, pClaimedHeader);

    /* Split: 
    We claim <need> bytes from the free block. Is the leftover large
    enough to file as a new free block? 

    If so, our claimed free block will only take <need> bytes, and file the leftover as a new block

    Otherwise, our claimed free block will keep its entire original size, with no leftover (leftover would be too small anyway) 
    */
    if (pClaimedHeader->size - need >= 2 * sizeof(BlockHeader))
    {
        /* Start leftover past the <need> bytes we kept */
        BlockHeader* pTailHeader = (BlockHeader*)((char*)pClaimedHeader + need);
        
        pClaimedFooter = pTailHeader - 1;

        // Populate info of new tail
        pTailHeader->size = pClaimedHeader->size - need;
        pTailHeader->prev = NULL;
        pTailHeader->next = NULL;
        pTailHeader->isFree = true;
        pClaimedHeader->size = need;    // update our size
        
        // Copy tail header into its footer
        BlockHeader* pTailFooter = (BlockHeader*)((char*)pTailHeader + pTailHeader->size - sizeof(BlockHeader));
        memcpy(pTailFooter, pTailHeader, sizeof(BlockHeader));   // add footer
        
        // Register the new tail
        insert_free_block(pTailHeader);
    }
    else
    {
        pClaimedFooter = (BlockHeader*)((char*)pClaimedHeader + pClaimedHeader->size - sizeof(BlockHeader));
    }

    // Copy our header into the newly created footer
    memcpy(pClaimedFooter, pClaimedHeader, sizeof(BlockHeader));
        

    /* Caller gets the payload, which starts after the header */
    return (char*)pClaimedHeader + sizeof(BlockHeader);
}

void* tlsf_realloc(void *ptr, uint32_t newSize)
{
    BlockHeader* pCurrHeader = (BlockHeader*)((char*)ptr - sizeof(BlockHeader));
    uint32_t need = (uint32_t)align(newSize + 2 * sizeof(BlockHeader));

    if (need == pCurrHeader->size)   // No resize, do nothing
    {
        return ptr;
    }

    BlockHeader* pCurrFooter = (BlockHeader*)((char*)pCurrHeader + pCurrHeader->size - sizeof(BlockHeader));

    /* Is this a shrinking resize? */
    if (need < pCurrHeader->size)
    {
        /* 
        Break current block into two: kept block (newSize)
        and a tail block */

        // Is the leftover tail block big enough to be a new block?
        if (pCurrHeader->size - need >= 2 * sizeof(BlockHeader))
        {
            // Set up tail block
            BlockHeader* pTailHeader = (BlockHeader*)((char*)pCurrHeader + need);
            pTailHeader->size = pCurrHeader->size - need;
            pTailHeader->prev = NULL;
            pTailHeader->next = NULL;
            pTailHeader->isFree = true;
            BlockHeader* pTailFooter = (BlockHeader*)((char*)pTailHeader + pTailHeader->size - sizeof(BlockHeader));
            memcpy(pTailFooter, pTailHeader, sizeof(BlockHeader));

            // Update current block
            BlockHeader* pCurrFooter = (BlockHeader*)((char*)pCurrHeader + need - sizeof(BlockHeader));
            pCurrHeader->size = need;
            memcpy(pCurrFooter, pCurrHeader, sizeof(BlockHeader));
        
            tlsf_free(pTailHeader + 1);
        }
        else
        {
            // do nothing
        }
    } 
    else    // Enlargening resize 
    {
        /* Check if we can simply carve out of right neighbour 
        (right neighbour leftover becomes 'tail')
        */
        BlockHeader* pRightHeader = pCurrFooter + 1;
        if (true == isHeaderInPoolRange(pRightHeader) 
            && true == pRightHeader->isFree
            && need <= pCurrHeader->size + pRightHeader->size)
        {
            uint32_t leftover = pCurrHeader->size + pRightHeader->size - need;
            // Move right free neighbour to possibly new free grid position
            uint32_t fl, sl;
            getLayerIndicesFromBlockSize(pRightHeader->size, &fl, &sl);
            popBlockHeaderNode(fl, sl, pRightHeader);
            BlockHeader* pRightFooter = (BlockHeader*)((char*)pRightHeader + pRightHeader->size - sizeof(BlockHeader));

            // We can carve out of right, but keep it as a free block
            if (leftover >= 2 * sizeof(BlockHeader))
            {
                // Change right neighbour (becomes 'tail'), now that we've carved it
                BlockHeader* pNewRightHeader = (BlockHeader*)((char*)pCurrHeader + need);
                pNewRightHeader->size = leftover;
                memcpy(pRightFooter, pNewRightHeader, sizeof(BlockHeader));
                
                insert_free_block(pNewRightHeader);

                // Update current block
                pCurrHeader->size = need;
                BlockHeader* pNewCurrFooter = (BlockHeader*)((char*)pCurrHeader + need - sizeof(BlockHeader));
                memcpy(pNewCurrFooter, pCurrHeader, sizeof(BlockHeader));
            
            }
            else    // Carve would leave right neighbour too small. Merge with right neighbour completely
            {
                pCurrHeader->size = pCurrHeader->size + pRightHeader->size;
                memcpy(pRightFooter, pCurrHeader, sizeof(BlockHeader));
            }
        }
        else
        {
            /* Run tlsf_malloc(newSize), copy bytes, and free this block */
            char* pNewlyClaimedSpace = tlsf_malloc(newSize);
            if (NULL == pNewlyClaimedSpace)
            {
                return NULL;
            }

            // Copy bytes over
            memcpy(pNewlyClaimedSpace, pCurrHeader + 1, pCurrHeader->size - 2 * sizeof(BlockHeader));

            // Free this region
            tlsf_free(pCurrHeader + 1);

            return pNewlyClaimedSpace;
        }
    }

    return ptr;
}

void tlsf_free(void *ptr)
{
    if (NULL == ptr)
    {
        return;
    }

    /* Find header associated with this region */
    BlockHeader* pCurrHeader = (BlockHeader*)((char *)ptr - sizeof(BlockHeader));
    BlockHeader* pCurrFooter = (BlockHeader*)((char*)pCurrHeader + pCurrHeader->size - sizeof(BlockHeader));
    /* 
    If left or right are free, we should merge 
    into a new, combined free block.
    Initialize new block assuming it's only the current block
    */
    BlockHeader* pNewHeader = pCurrHeader;
    BlockHeader* pNewFooter = pCurrFooter;
    uint32_t newSize = pCurrHeader->size;
    
    BlockHeader* pLeftFooter = pCurrHeader - 1;
    if (true == isHeaderInPoolRange(pLeftFooter) 
        && true == pLeftFooter->isFree)
    {
        BlockHeader* pLeftHeader = (BlockHeader*)((char*)pCurrHeader - pLeftFooter->size); 
        assert(true == isHeaderInPoolRange(pLeftHeader));
        
        uint32_t fl, sl;
        getLayerIndicesFromBlockSize(pLeftHeader->size, &fl, &sl);
        popBlockHeaderNode(fl, sl, pLeftHeader);
        
        pNewHeader = pLeftHeader;
        newSize += pLeftHeader->size;
    }
    

    BlockHeader* pRightHeader = pCurrFooter + 1;

    if (true == isHeaderInPoolRange(pRightHeader) 
        && true == pRightHeader->isFree)
    {
        BlockHeader* pRightFooter = (BlockHeader*)((char*)pRightHeader + pRightHeader->size - sizeof(BlockHeader)); 
        assert(true == isHeaderInPoolRange(pRightFooter));
        
        uint32_t fl, sl;
        getLayerIndicesFromBlockSize(pRightHeader->size, &fl, &sl);
        popBlockHeaderNode(fl, sl, pRightHeader);
        
        pNewFooter = pRightFooter;
        newSize += pRightFooter->size;
    }

    /* Update new block info */
    pNewHeader->size = newSize;
    pNewHeader->isFree = true;
    pNewHeader->prev = NULL; 
    pNewHeader->next = NULL;

    memcpy(pNewFooter, pNewHeader, sizeof(BlockHeader));

    insert_free_block(pNewHeader);
}

/* 
Pop <node> from the linked list at free_grid[fl][sl]

Assumes <node> is an element of the list
*/
void popBlockHeaderNode( uint32_t fl, uint32_t sl, BlockHeader* node)
{
    node->isFree = false;

    /* Is this node the head? */
    if (NULL == node->prev)
    {
        tlsf.free_grid[fl][sl] = node->next;
        
        if (NULL != node->next)
        {
            node->next->prev = NULL;
        }
        else // Grid is empty now
        {
            // Update bitmaps
            tlsf.secondLayerBitmap[fl] &= ~(1u << sl);
            if (0 == tlsf.secondLayerBitmap[fl])
            {
                tlsf.firstLayerBitmap &= ~(1u << fl);
            }
        }
        
        node->next = NULL;
        return;
    }

    /* a - node - b becomes a - b */
    node->prev->next = node->next;

    /* Is this node not the tail? */
    if (NULL != node->next)
    {
        node->next->prev = node->prev;
    }

    return;
}

bool isHeaderInPoolRange(BlockHeader* pBlockHeader)
{
    return (char*)tlsf.pool <= (char*)pBlockHeader 
    && (char*)(pBlockHeader + 1) <= (char*)tlsf.pool + tlsf.poolSize; 
}
