//header file
#include<stdint.h>

#define NUM_SIZE_CLASS 7
const int size_classes_avail[] = {4, 8, 16, 32, 64, 128, 512};   //Make sure to update NUM_SIZE_CLASS if updating this.

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
typedef struct 
{
    uint32_t avail;
    uint32_t count;
    anchor_state_e state;
    uint32_t tag;
}anchor_t;

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
    processor_heap_t heap;      //pointer to owner processor heap
    uint32_t block_size;        //size of block
    uint32_t maxcount;          //superblock size
}descriptor_t;

#define true 1
#define false 0
#define FAILURE -1
