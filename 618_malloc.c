#include<stdio.h>
#include<errno.h>
#include<pthread.h>
#include<unistd.h>
#include<stdlib.h>
#include "618_malloc.h"
#include <sys/mman.h>
/* Global declerations */
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Allocate size classes */
static sizeclass_t size_class[NUM_SIZE_CLASS];
static processor_heap_t *proc_heap = NULL;
#define PROC_HEAP_OFFSET(processor, size_class) (processor * NUM_SIZE_CLASS) + size_class

#define INDEX 0
#define ADDRESS 1
#define ALIGNMENT 8
/* Define init lock. Can use a lock for init because it is one
 * time activity. And unless init is complete, no thread can proceedc */
pthread_mutex_t init_lock;

static int initialized = false;
static int32_t num_processors = 0;
static descriptor_t *desc_avail = NULL;
static uint32_t desc_avail_count = 0;

#define MIN(x,y) ( (x > y) ? y : x )

static size_t align(size_t x)
{
    return ((ALIGNMENT* (x+ALIGNMENT -1)/ALIGNMENT)) + EIGHTBYTES;
}


int8_t find_heap_index(size_t req_size)
{
    int cpu = sched_getcpu();
    int i = 0;

    for(i = 0; i < NUM_SIZE_CLASS; i++)
    {
        if((req_size+8) < (size_t)size_class[i].block_size)
        {
            return (cpu * NUM_SIZE_CLASS) + i;
        }
    }

    return -1;
}


void *alloc_from_sys(size_t req_size)
{
    req_size = align(req_size);
    void *result = mmap (NULL, req_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if(result == MAP_FAILED)
    {
        return NULL;
    }
    
    *(int *)result = req_size;
    return ((char*)result+EIGHTBYTES);
}

uint64_t get_anchor_avail(uint64_t anchor)
{
   return (anchor & 0xFFC0000000000000) >> 54;
}

anchor_t set_anchor_avail(anchor_t anchor, uint64_t val)
{
    return (anchor_t)(((uint64_t)anchor & 0x003FFFFFFFFFFFFF) 
                | (val << 54));
}

uint64_t get_anchor_count(uint64_t anchor)
{
   return (anchor & 0x003FF00000000000) >> 44;
}

anchor_t set_anchor_count(anchor_t anchor, uint64_t val)
{
    return (anchor_t)(((uint64_t)anchor & 0xFFC00FFFFFFFFFFF) 
                | (val << 44));
}

uint64_t get_anchor_state(uint64_t anchor)
{
    return (anchor & 0x00000C0000000000) >> 42;
}

anchor_t set_anchor_state(anchor_t anchor, uint64_t val)
{
    return (anchor_t)(((uint64_t)anchor & 0xFFFFF3FFFFFFFFFF) 
               | (val << 42));
}

uint64_t get_anchor_tag(uint64_t anchor)
{
    return (anchor & 0x000003FFFFFFFFFF);
}

anchor_t set_anchor_tag(anchor_t anchor, uint64_t val)
{
    return (anchor_t)(((uint64_t)anchor & 0xFFFFFC0000000000) 
               | val);
}

/* FIXME: ----------   Aditya there is an ABA problem in this function */
/* Put desc at the end of the list */
void list_put_partial(sizeclass_t *size_class, descriptor_t *desc)
{
    while(1)
    {
        descriptor_t *new_list = size_class->partial.tail;
        descriptor_t *old_list = size_class->partial.tail;

        /* Try to update tail->next first */
        if(old_list->next == NULL)
        {
            while(1)
            {
                if(__sync_bool_compare_and_swap(&old_list->next, NULL, desc))
                {
                    break;   
                }
            }
        }
        new_list = desc;
        if(__sync_bool_compare_and_swap(&size_class->partial.tail, old_list, new_list))
        {
            break;   
        }
    }
    //return desc;
}

void heap_put_partial(descriptor_t *desc)
{
    descriptor_t * prev = NULL;
    while(1)
    {
        prev = desc->heap->partial;
        if(__sync_bool_compare_and_swap(&desc->heap->partial, prev, desc))
        {
            break;
        }
    }
    if(prev)
    {
        list_put_partial(desc->heap->size_class,prev);
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* Initialize malloc implementation 
 * 1. Create sizeclasses
 * 2. Each sizeclass will have "num_processors" heaps 
 */
void init(void)
{
    int loop_cnt = 0;

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
       // proc_heap = (processor_heap_t *)sbrk(sizeof(processor_heap_t)*total_proc_heaps);
        proc_heap = (processor_heap_t *)mmap( NULL, sizeof(processor_heap_t)*total_proc_heaps, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);


        printf("Mmap done = %p\n", proc_heap);

        if(proc_heap == MAP_FAILED)
        {
            return;
        }

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
   // add = sbrk(size*blocks);
    if((add = mmap( NULL, size*blocks, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)) == MAP_FAILED)
    {
        return NULL;
    }
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
    uint64_t count = 0;

    new_active = (active_t*)((uint64_t)new_active & 0xFFFFFFFFFFFFFFC0);
    new_active = (active_t *)((uint64_t)new_active |(morecredits - 1));

    if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].active, NULL, new_active))
    {
        return;
    }

    /* Damn. Someone installed another SB */
    while(1)
    {
        new_anchor = old_anchor = desc->anchor;

        count = get_anchor_count(new_anchor);
        count += count+morecredits;
        new_anchor = set_anchor_count(new_anchor, count);
        
        new_anchor = set_anchor_state(new_anchor,(uint64_t)PARTIAL);

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

            /* FIXME: This won't work... Need to put this in an atomic block. 
             * Addresses are 64 bit!!! ADITYA -----------
             * Use atomic - AVR */
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
            desc = alloc_new_superblk(sizeof(descriptor_t), total_proc_heaps, ADDRESS);       //FIXME: Is this size fit?
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
#if 0
                void *current = sbrk(0);
                if(FAILURE != brk(desc+(sizeof(descriptor_t)*total_proc_heaps)))
                {
                    sbrk(0 - (sizeof(descriptor_t)*total_proc_heaps));
                    brk(current);
                }
#endif
                if(munmap(desc,(size_t)(sizeof(descriptor_t)*total_proc_heaps)) == FAILURE)
                {
                    return NULL;
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
        descriptor_t *new_list = size_class->partial.head;
        descriptor_t *old_list = size_class->partial.head;
    
        desc = new_list;

        new_list = old_list->next;
        //new_list.tag++;               //Ignoring the ABA problem here 
        
        if(__sync_bool_compare_and_swap(&size_class->partial.head, old_list, new_list))
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
        desc = proc_heap[proc_heap_index].partial;
        if(desc == NULL)
        {
            /* FIXME: WTF is list get partial???? ADITYA */
            return list_get_partial(proc_heap[proc_heap_index].size_class);
        }

        if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].partial, desc, NULL))
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
    void *addr = NULL;
    uint64_t count = 0;

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

        if(get_anchor_state(old_anchor) == EMPTY)
        {
            desc_retire(desc);
            goto retry;
        }

        /* Old anchor state must be PARTIAL and old anchor count must be > 0 */
        morecredits = MIN(get_anchor_count(old_anchor)- 1, MAXCREDITS);

        count = get_anchor_count(new_anchor);
        count -= morecredits + 1;
        new_anchor = set_anchor_count(new_anchor, count);

        new_anchor = set_anchor_state(new_anchor, (uint64_t)((morecredits > 0) ? ACTIVE : FULL));

        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
    }

    /* Pop the reserved block */
    while(1)
    {
        new_anchor = old_anchor = desc->anchor;
        //addr = desc->super_block + GET_ANCHOR_AVAIL((uint64_t)old_anchor)*desc->block_size;
        addr = desc->super_block + get_anchor_avail((uint64_t)old_anchor)*desc->block_size;
        new_anchor = set_anchor_avail(new_anchor, (uint64_t)*(unsigned *)addr);
        
        count = get_anchor_tag(new_anchor);
        count++;
        new_anchor = set_anchor_tag(new_anchor, count);

        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }

        if(morecredits > 0)
        {
            update_active(proc_heap_index, desc, morecredits);
        }

        *(descriptor_t **)addr = desc;
        return ((char *)addr + EIGHTBYTES);
    }
}

/* Malloc from new Active block                  
 * This function will provide a block from the ACTIVE
 * super block. */
void *malloc_from_active(int8_t proc_heap_index)
{
    descriptor_t *desc = NULL;
    void *addr = NULL;
    uint64_t temp = 0;
    uint16_t next = 0;
    uint32_t morecredits = 0;
    active_t *new_active;
    active_t *old_active;

    while(1)
    {
        /* Reserve a block */
        new_active = proc_heap[proc_heap_index].active;
        old_active = proc_heap[proc_heap_index].active;
        
        if(!old_active)
        {
            return NULL;    
        }

        if(0 == ((uint64_t)old_active & 0x000000000000003F))
        {
            new_active = NULL;
        }
        else
        {
            uint8_t temp = (uint8_t)((uint64_t)new_active & 0x000000000000003F);
            temp -= 1;
            new_active = (active_t *)((uint64_t)new_active & 0xFFFFFFFFFFFFFFC0);
            new_active = (active_t *)((uint64_t)new_active|temp);
            //new_active.credits--;
        }

        if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].active, old_active, new_active))
        {
            break;
        }
    }
    /* Block is reserved. Pop the block */
    //desc = mask_credits(old_active);        //FIXME: Aditya -- WTF is this....think about it!
    desc = (descriptor_t *)((uint64_t)old_active & 0xFFFFFFFFFFFFC0);


    anchor_t new_anchor;
    anchor_t old_anchor;
    while(1)
    {
        new_anchor = desc->anchor;
        old_anchor = desc->anchor;

        addr = (void *)(desc->super_block + get_anchor_avail((uint64_t)old_anchor) * desc->block_size);

        next = *(unsigned *)addr;

        new_anchor = set_anchor_avail((uint64_t)new_anchor, (uint64_t)next);
        temp = get_anchor_tag(new_anchor);
        temp++;
        new_anchor = set_anchor_tag(new_anchor, temp);

        //if(0 == old_anchor.credits)
        if(0 == ((uint64_t)old_active & 0x000000000000003F))
        {
            /* State must be ACTIVE */
            if(0 == get_anchor_count(old_anchor))
            {
                new_anchor = set_anchor_state((uint64_t)new_anchor, (uint64_t)FULL);
            }
            else
            {
                morecredits = MIN(get_anchor_count(old_anchor), MAXCREDITS);
            }
            temp = get_anchor_count(new_anchor);
            temp -= morecredits;
            new_anchor = set_anchor_count(new_anchor, temp);
        }

        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
    }

    if((0 == ((uint64_t)old_active & 0x000000000000003F)) && (get_anchor_count(old_anchor) > 0))
    {
        update_active(proc_heap_index, desc, morecredits);
    }

    *(descriptor_t **)addr = desc;

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
  
    desc->super_block = alloc_new_superblk(proc_heap[proc_heap_index].size_class->block_size, \
                (proc_heap[proc_heap_index].size_class->sb_size/proc_heap[proc_heap_index].size_class->block_size), INDEX); 

    desc->heap = &proc_heap[proc_heap_index];
    //desc->anchor = SET_ANCHOR_AVAIL(desc->anchor, 1);
    desc->anchor = set_anchor_avail(desc->anchor, 1);

    desc->block_size = proc_heap[proc_heap_index].size_class->block_size;
    desc->maxcount = proc_heap[proc_heap_index].size_class->sb_size/desc->block_size;

    new_active = desc;
    new_active = (active_t *)((uint64_t)new_active & 0xFFFFFFFFFFFFC0);
    new_active = (active_t *)((uint64_t)new_active | MIN(desc->maxcount - 1, MAXCREDITS) - 1);
    //desc->anchor = SET_ANCHOR_COUNT(desc->anchor, (desc->maxcount - 1) - (((uint64_t)new_active & 0xFFFFFFFFFFFFC0) + 1));
    //desc->anchor = SET_ANCHOR_STATE(desc->anchor, ACTIVE);
    desc->anchor = set_anchor_count(desc->anchor, (desc->maxcount - 1) - (((uint64_t)new_active & 0xFFFFFFFFFFFFC0) + 1));
    desc->anchor = set_anchor_state(desc->anchor, ACTIVE);

    if(__sync_bool_compare_and_swap(&proc_heap[proc_heap_index].active, NULL, new_active))
    {
        addr = desc->super_block;
        *(descriptor_t **)addr = desc;
        return addr + EIGHTBYTES;
    }
    else
    {
        /* Someone else allocated a SB :O 
         * Return this SB to the OS. FIXME: Is this
         * how we return? FIXME FIXME FIXME */
        /* High chance you might run into an error: Check sizes and pointers */
#if 0
        void *current = sbrk(0);

        if(FAILURE != brk(desc->super_block+(proc_heap[proc_heap_index].size_class->sb_size)))
        {
            sbrk(0 - (proc_heap[proc_heap_index].size_class->sb_size));
            brk(current);
        }
        desc_retire(desc);
#endif
        if(munmap(desc->super_block,(size_t)(proc_heap[proc_heap_index].size_class->sb_size)) == FAILURE)
        {
            return NULL;
        }
    }
    return NULL;
}




/* Malloc function */
void *malloc(size_t req_size)
{
    printf(">>>>>>>>>>>>>>>>>>>>>>><<<<<<<<<<<<<<<<<<<\n");
    int8_t proc_heap_index = 0;
    void *addr = NULL;

    if(!initialized)
    {
        init();       
    }
    printf("Loozy = %d\n", initialized);
    
    proc_heap_index = find_heap_index(req_size);
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


void list_remove_empty_desc(sizeclass_t *size_class)
{
    while(1)
    {
        desc_list_t new_list = size_class->partial;
        desc_list_t old_list = size_class->partial;

        descriptor_t *oldhead = old_list.head;
        new_list.head = oldhead->next;
        if(__sync_bool_compare_and_swap(&old_list.head, oldhead, new_list.head))
        {
            break;
        }
    }
}

void remove_empty_desc(processor_heap_t *heap,descriptor_t *desc)
{
    if(__sync_bool_compare_and_swap(&heap->partial,desc,NULL))
    {
        desc_retire(desc);
    }
    else
    {
        list_remove_empty_desc(heap->size_class);
    }
}

void free(void * ptr)
{
    descriptor_t * desc = NULL;
    void * sb   = NULL;
    processor_heap_t * heap;

    anchor_t new_anchor, old_anchor;

    if(ptr == NULL)
    {
        return;
    }
    (void**)ptr--;
    desc = *(descriptor_t**)ptr;


    if(large_block_bit_set(desc))
    {
        //Return block to OS --- ???
       size_t block_size = (size_t)desc;

       if(munmap(desc,block_size) == FAILURE)
       {
            return;
       }
    }

    sb = desc->super_block;
    while(1)
    {
        new_anchor = set_anchor_avail(new_anchor,(ptr - sb) / desc->block_size);
        if(get_anchor_state(old_anchor) == FULL)
        {
           new_anchor = set_anchor_state(new_anchor,PARTIAL);
        }
        if(get_anchor_count(old_anchor) == desc->maxcount-1)
        {
            heap = desc->heap;
            //TODO: Add instruction fence here to make sure desc->head read completes moving ahead
           new_anchor = set_anchor_state(new_anchor,EMPTY);
        }
        else
        {
            new_anchor = set_anchor_count(new_anchor, get_anchor_count(new_anchor)+1);
        }
        //TODO: Add memory fence
        if(__sync_bool_compare_and_swap(&desc->anchor, old_anchor, new_anchor))
        {
            break;
        }
    }
    if(get_anchor_state(new_anchor) == EMPTY)
    {
        //Free the superblock sb
#if 0
        void *current = sbrk(0);

        if(FAILURE != brk(desc->super_block+(proc_heap[proc_heap_index].size_class->sb_size)))
        {
            sbrk(0 - (proc_heap[proc_heap_index].size_class->sb_size));
            brk(current);
            desc->super_block = NULL;
        }
        remove_empty_desc(heap,desc);
#endif
        
         //if(munmap(desc->super_block,(size_t)(proc_heap[proc_heap_index].size_class->sb_size)) == FAILURE)
         if(munmap(desc->super_block,(size_t)(desc->heap->size_class->sb_size)) == FAILURE)
         {
             return;
         }
    }
    else if(get_anchor_state(old_anchor) == FULL)
    {
        heap_put_partial(desc);
    }
    return;
}


int large_block_bit_set(descriptor_t *desc)
{
    if(*(uint64_t *)desc & (uint64_t)0x01)
    {
        return 1;
    }
    return 0;
}
