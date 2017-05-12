//header file
#include<stdint.h>

#define NUM_SIZE_CLASS 7
const int size_classes_avail[] = {16, 32, 64, 128, 512, 1024, 2048};   //Make sure to update NUM_SIZE_CLASS if updating this.
#define MAXCREDITS 1 << 6
#define EIGHTBYTES 8

void *malloc(size_t size);
void free(void *ptr);

/* Anchor state enum */
typedef enum
{
    ACTIVE = 0,
    FULL,
    PARTIAL,
    EMPTY
}anchor_state_e;

/* For ease of use - define an enum variable */
anchor_state_e anchor_state;

/* Anchor structure */
#if 0
typedef struct 
{
    uint32_t avail;
    uint32_t count;
    anchor_state_e state;
    uint32_t tag;
}anchor_t;
#endif

typedef uint64_t anchor_t;      //avail:10, count:10, state:2, tag:42
/* Extract anchor data */
#define GET_ANCHOR_AVAIL(x) (x & 0xFFC0000000000000) >> 54
#define SET_ANCHOR_AVAIL(x, y) (x & 0x003FFFFFFFFFFFFF) | (y << 54)

#define GET_ANCHOR_COUNT(x) (x & 0x003FF00000000000) >> 44
#define SET_ANCHOR_COUNT(x, y) (x & 0xFFC00FFFFFFFFFFF) | (y << 44)

#define GET_ANCHOR_STATE(x) (x & 0x00000C0000000000) >> 42
#define SET_ANCHOR_STATE(x, y) (x & 0xFFFFF3FFFFFFFFFF) | (y << 42)

#define GET_ANCHOR_TAG(x) (x & 0x000003FFFFFFFFFF)
#define SET_ANCHOR_TAG(x, y) (x & 0xFFFFFC0000000000) | y

struct descriptor; 

/* Active superblock structure */
#if 0
typedef struct
{
    struct descriptor *ptr;      //pointer to active superblock
    uint32_t credits;
}active_t;
#endif

typedef struct descriptor active_t;      //63 - 6 -> address 5 - 0 -> credits

typedef struct 
{
    struct descriptor *head;
    struct descriptor *tail;
    uint32_t tag;
} desc_list_t;

/* Sizeclass structure */
typedef struct
{
    desc_list_t partial;
    uint32_t block_size;
    uint32_t sb_size;       //size of superblock
}sizeclass_t;


/* Processor heap structure */
typedef struct 
{
    active_t *active;
    struct descriptor *partial;
    sizeclass_t *size_class;
}processor_heap_t;

/* Descriptor structure */
typedef struct descriptor
{
    anchor_t anchor;
    struct descriptor *next;
    void *super_block;          //pointer to superblock
    processor_heap_t *heap;      //pointer to owner processor heap
    uint32_t block_size;        //size of block
    uint32_t maxcount;          //superblock size
}descriptor_t;

#define true 1
#define false 0
#define FAILURE -1
