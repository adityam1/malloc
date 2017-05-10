#include<stdio.h>
#include<errno.h>
#include<pthread.h>
#include<unistd.h>
#include<stdlib.h>
#include "618_malloc.h"

/* Global declerations */
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Allocate size classes */
static sizeclass_t size_class[NUM_SIZE_CLASS];
static processor_heap_t *proc_heap = NULL;
#define PROC_HEAP_OFFSET(processor, size_class) (processor * NUM_SIZE_CLASS) + size_class

#define INDEX 0
#define ADDRESS 1

/* Define init lock. Can use a lock for init because it is one
 * time activity. And unless init is complete, no thread can proceedc */
pthread_mutex_t init_lock;

int initialized = false;

descriptor_t *desc_avail = NULL;
static uint32_t desc_avail_count = 0;

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Initialize malloc implementation 
 * 1. Create sizeclasses
 * 2. Each sizeclass will have "num_processors" heaps 
 */
void init(void)
{
    int loop_cnt = 0;
    int32_t num_processors = 0;

    pthread_mutex_lock(&init_lock);
    if(!initialized)
    {
        /* Get number of processors on the machine */
        num_processors = sysconf(_SC_NPROCESSORS_CONF);

        /* Update Size class structure */
        for(loop_cnt = 0; loop_cnt < NUM_SIZE_CLASS; loop_cnt++)
        {
            /* size is in bytes */
            size_class[loop_cnt].block_size = size_classes_avail[loop_cnt]; 
            size_class[loop_cnt].sb_size = 16384; //16KB - all superblocks are 16KB FIXME. Why?  
            size_class[loop_cnt].partial.head = NULL;
            size_class[loop_cnt].partial.tail = NULL;
            size_class[loop_cnt].partial.tag = 0;
        }
        /* Allocate processor heaps */
        size_t total_proc_heaps = num_processors * NUM_SIZE_CLASS; //NUM_SIZE_CLASS heaps for each processor
        
        /* FIXME: Should use sbrk????? */
        //proc_heap = (processor_heap_t *)malloc(sizeof(processor_heap_t)*total_proc_heaps);
        proc_heap = (processor_heap_t *)sbrk(sizeof(processor_heap_t)*total_proc_heaps);

        /* Arranging heaps per processor
         *
         *  sc => sizeclass
         *              ---------------------------- 
         * processor 1  | sc 1 | sc 2 | ... | sc N |
         *              ----------------------------
         * processor 2  | sc 1 | sc 2 | ... | sc N | 
         *              ----------------------------
         *              .   .   .   .   .   .   .  .   
         *              .   .   .   .   .   .   .  .   
         *              .   .   .   .   .   .   .  .   
         *              ----------------------------
         * processor N  | sc 1 | sc 2 | ... | sc N | 
         *              ----------------------------
         */
        for(loop_cnt = 0; loop_cnt < num_processors; loop_cnt++)
        {
            int i = 0;
            for(i = 0; i < NUM_SIZE_CLASS; i++)
            {
                /* Pointer to parent size class */
                int index = (loop_cnt * NUM_SIZE_CLASS) + i;
                proc_heap[index].size_class = &size_class[i];
            }

            /* Everything else is NULL at this time */
            proc_heap[loop_cnt].active = NULL;
            proc_heap[loop_cnt].partial = NULL;
        }

        initialized = true;
    }
    pthread_mutex_unlock(&init_lock);
}

void *alloc_new_superblk(size_t size, uint32_t blocks, uint8_t init_type)
{
    void *add = NULL;
    uint32_t loop_cnt = 0;
    descriptor_t *address = NULL;
    add = sbrk(size*blocks);
    char *data = NULL;

    if(0 != errno)
    {
        return NULL;
    }
    
    if(ADDRESS == init_type)
    {
        address = (descriptor_t *)add;
    }
    else
    {
        data = (char *)add;   
    }
    

    /* Need to organise these descriptor allocations in a linked list */
    for(loop_cnt = 0; loop_cnt < blocks; loop_cnt++)
    {
        if(ADDRESS == init_type)
        {
            if(loop_cnt != blocks - 1)
            {
                address[loop_cnt].next = &address[loop_cnt+1];
            }
            else
            {
                address[loop_cnt].next = NULL;
            }
        }
        else
        {
            if(loop_cnt != blocks - 1)
            {
                data[loop_cnt * size] = loop_cnt + 1;    
            }
        }
    }
    return address;
}

void update_active(int8_t proc_heap_index, descriptor_t *desc, int morecredits)
{
    active_t *new_active = desc;
    anchor_t new_anchor, old_anchor;

    new_active &= 0xFFFFFFFFFFFFFFC0;
    new_active |= morecredits - 1;
    if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index]->active, NULL, new_active))
    {
        return;
    }

    /* Damn. Someone installed another SB */
    while(1)
    {
        new_anchor = old_anchor = desc->anchor;
        new_anchor.count  += morecredits;
        new_anchor.state = PARTIAL;
        
        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
        heap_put_partial(desc);
    }
}

/* Retire a descriptor i.e. add it to the free desc list */
void desc_retire(descriptor_t *desc)
{
    descriptor_t *head = NULL;

    while(1)
    {
        head = desc_avail;
        desc->next = head;
        if(__sync_bool_compare_and_swap(&desc_avail, head, desc))
        {
            break;
        }
    }
}

/* Allocate a descriptor from a list of available discriptors */
descriptor_t *desc_alloc()
{
    descriptor_t *desc = NULL;
    int loop_cnt = 0;

    while(1)
    {
        uint64_t new, old;
        desc = desc_avail;
        if(NULL != desc)
        {
            int count = desc_avail_count;
            descriptor_t *next = desc->next;
            
            /* for the sake of a double CAS */
            uint64_t old = (uint64_t)desc;
            old = old << 32;
            old |= count;
            
            uint64_t new = (uint64_t)next;
            new = new << 32;
            new |= count+1;

            if(__sync_bool_compare_and_swap(&desc_avail, old, new))
            {
                break;   
            }
        }
        else
        {
            /* Request for a superblock for descriptors */
            size_t total_proc_heaps = num_processors * NUM_SIZE_CLASS; //NUM_SIZE_CLASS heaps for each processor
            desc = alloc_new_superblk(sizeof(descriptor_t), total_proc_heap, ADDRESS);       //FIXME: Is this size fit?
            if(desc == NULL)
            {
                /* Failed to allocate memory */
                return NULL;
            }

            /* Add this to the global pointer */
            if(__sync_bool_compare_and_swap(&desc_avail, NULL, desc->next))
            {
                break;
            }
            else
            {
                /* Someone else allocated a SB :O 
                 * Return this SB to the OS. FIXME: Is this
                 * how we return? FIXME FIXME FIXME */
                void *current = sbrk(0);
                if(FAILURE != brk(desc+(sizeof(descriptor_t)*total_proc_heap)))
                {
                    sbrk(0 - (sizeof(descriptor_t)*total_proc_heap));
                    brk(current);
                }
            }
        }
    }
    return desc;
}

/* Get desc from the head of the list */
descriptor_t *list_get_partial(sizeclass_t *size_class)
{
    descriptor_t *desc = NULL;
    while(1)
    {
        desc_list_t new_list = size_class->partial;
        desc_list_t old_list = size_class->partial;
    
        desc = new_list->partial.head;

        new_list.head = old_list.head->next;
        new_list.tag++;
        
        if(__sync_bool_compare_and_swap(&size_class->partial, old_list, new_list))
        {
            break;   
        }
    }
    return desc;
}

/* FIXME: ----------   Aditya there is an ABA problem in this function */
/* Put desc at the end of the list */
descriptor_t *list_put_partial(sizeclass_t *size_class, descriptor_t *desc)
{
    while(1)
    {
        desc_list_t new_list = size_class->partial;
        desc_list_t old_list = size_class->partial;

        /* Try to update tail->next first */
        if(old_list->tail->next == NULL)
        {
            while(1)
            {
                if(__sync_bool_compare_and_swap(&old_list->tail->next, NULL, desc))
                {
                    break;   
                }
            }
        }
        new_list.tail = desc;
        new_list.tag++;
        if(__sync_bool_compare_and_swap(&size_class->partial, old_list, new_list))
        {
            break;   
        }
    }
    return desc;
}

descriptor_t *heap_get_partial(int8_t proc_heap_index)
{
    descriptor_t *desc = NULL;

    while(1)
    {
        desc = proc_heap[proc_heap_index]->partial;
        if(desc == NULL)
        {
            /* FIXME: WTF is list get partial???? ADITYA */
            return list_get_partial(proc_heap[proc_heap_index]->size_class);
        }

        if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index]->partial, desc, NULL))
        {
            break;
        }
    }
    return desc;
}

/* Malloc from a partial block */
void *malloc_from_partial(int8_t proc_heap_index)
{
    descriptor_t *desc = NULL;
    anchor_t new_anchor, old_anchor;
    int32_t morecredits = 0;
    void *addr = NULL:

retry:
    desc = heap_get_partial(proc_heap_index);
    if(NULL == desc)
    {
        return NULL;
    }

    desc->heap = &proc_heap[proc_heap_index];

    while(1)
    {
        /* Reserve blocks */
        new_anchor = old_anchor = desc->anchor;

        if(old_anchor.state == EMPTY)
        {
            desc_retire(desc);
            goto retry;
        }

        /* Old anchor state must be PARTIAL and old anchor count must be > 0 */
        morecredits = min(old_anchor.count - 1, MAXCREDITS);

        new_anchor.count -= morecredits + 1;

        new_anchor.state = (morecredits > 0) ? ACTIVE : FULL;

        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
    }

    /* Pop the reserved block */
    while(1)
    {
        new_anchor = old_anchor = desc->anchor;
        addr = desc->super_block + old_anchor.avail*desc->block_size;
        new_anchor.avail = *(unsigned *)addr;
        new_anchor.tag++;
        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }

        if(morecredits > 0)
        {
            update_active(&proc_heap[proc_heap_index], desc, morecredits);
        }

        *addr = desc;
        return addr+EIGHTBYTES;
    }
}

/* Malloc from new Active block                  
 * This function will provide a block from the ACTIVE
 * super block. */
void *malloc_from_active(int8_t proc_heap_index)
{
    descriptor_t *desc = NULL;
    while(1)
    {
        /* Reserve a block */
        active_t new_active = heap->active;
        active_t old_active = heap->active;
        
        if(!old_active)
        {
            return NULL;    
        }

        if(0 == (old_active & 0x000000000000003F))
        {
            new_active = NULL;
        }
        else
        {
            uint8_t temp = new_active & 0xFFFFFFFFFFFFFFC0;
            temp -= 1;
            new_active &= 0xFFFFFFFFFFFFFFC0;
            new_active |= temp;
            //new_active.credits--;
        }

        if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].active, old_active, new_active))
        {
            break;
        }


        /* Block is reserved. Pop the block */
        //desc = mask_credits(old_active);        //FIXME: Aditya -- WTF is this....think about it!
        desc = old_active & 0xFFFFFFFFFFFFC0;

        while(1)
        {
            anchor_t new_anchor = desc->anchor;
            anchor_t old_anchor = desc->anchor;

            addr = desc->super_block + old_anchor.avail * desc->size_class;

            next = *(unsigned *)addr;

            new_anchor.avail = next;
            new_anchor.tag++;

            //if(0 == old_anchor.credits)
            if(0 == (old_active & 0x000000000000003F))
            {
                /* State must be ACTIVE */
                if(0 == old_anchor.count)
                {
                    new_anchor.state = FULL;
                }
                else
                {
                    morecredits = min(old_anchor.count, MAXCREDITS);
                }
                new_anchor.count -=morecredits;
            }
        }

        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
    }

    if((0 == (old_active & 0x000000000000003F)) && (old_anchor.count > 0))
    {
        update_active(&proc_heap[proc_heap_index], desc, morecredits);
    }

    *addr = desc;

    return addr + EIGHTBYTES;
}

/* Malloc from new Super block                  
 * This function will allocate a new superblocka and provide a block from 
 * that super block. */
void *malloc_from_newsb(int8_t proc_heap_index)
{
    void *addr = NULL;
    descriptor_t *desc = desc_alloc();
    active_t *new_active = NULL;
  
    desc->super_block = alloc_new_superblk(proc_heap[proc_heap_index]->size_class, \
                (proc_heap[proc_heap_index]->size_class->sb_size/proc_heap[proc_heap_index]->size_class), INDEX); 

    desc->heap = &proc_heap[proc_heap_index];
    desc->anchor.avail = 1;

    desc->block_size = proc_heap[proc_heap_index]->size_class->block_size;
    desc->maxcount = proc_heap[proc_heap_index]->size_class->sb_size/desc->block_size;

    new_active = desc;
    new_active &= 0xFFFFFFFFFFFFC0;
    new_active |= min(desc->maxcount - 1, MAXCREDITS) - 1;
    desc->anchor.count = (desc->maxcount - 1) - ((new_active & 0xFFFFFFFFFFFFC0) + 1);
    desc->anchor.state = ACTIVE;

    if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].active, NULL, new_active))
    {
        addr = desc->super_block;
        *addr = desc;
        return addr + EIGHTBYTES;
    }
    else
    {
        /* Someone else allocated a SB :O 
         * Return this SB to the OS. FIXME: Is this
         * how we return? FIXME FIXME FIXME */
        /* High chance you might run into an error: Check sizes and pointers */

        void *current = sbrk(0);

        if(FAILURE != brk(desc->super_block+(proc_heap[proc_heap_index]->size_class->sb_size)))
        {
            sbrk(0 - (proc_heap[proc_heap_index]->size_class->sb_size));
            brk(current);
        }
        desc_retire(desc);
    }
    return NULL;
}

/* Malloc function */
void *malloc(size_t req_size)
{
    int8_t proc_heap_index = 0;
    void *addr = NULL:

    if(!initialized)
    {
        init();       
    }
    
    proc_heap_index = find_heap(req_size);
    if(FAILURE == proc_heap_index)
    {
        return alloc_from_sys(req_size);
    }

    while(1)
    {
        addr = malloc_from_active(proc_heap_index);
        if(addr != NULL)
        {
            return addr;
        }

        addr = malloc_from_partial(proc_heap_index);
        if(addr != NULL)
        {
            return addr;
        }

        addr = malloc_from_newsb(proc_heap_index);
        if(addr != NULL)
        {
            return addr;
        }       
    }
}
