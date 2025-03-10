/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * File      : memheap.c
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-04-10     Bernard      first implementation
 * 2012-10-16     Bernard      add the mutex lock for heap object.
 * 2012-12-29     Bernard      memheap can be used as system heap.
 *                             change mutex lock to semaphore lock.
 * 2013-04-10     Bernard      add rt_memheap_realloc function.
 * 2013-05-24     Bernard      fix the rt_memheap_realloc issue.
 * 2013-07-11     Grissiom     fix the memory block splitting issue.
 * 2013-07-15     Grissiom     optimize rt_memheap_realloc
 * 2021-06-03     Flybreak     Fix the crash problem after opening Oz optimization on ac6.
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MEMHEAP

/* dynamic pool magic and mask */
#define RT_MEMHEAP_MAGIC        0x1ea01ea0
#define RT_MEMHEAP_MASK         0xfffffffe
#define RT_MEMHEAP_USED         0x01
#define RT_MEMHEAP_FREED        0x00

#define RT_MEMHEAP_IS_USED(i)   ((i)->magic & RT_MEMHEAP_USED)
#define RT_MEMHEAP_MINIALLOC    12

#define RT_MEMHEAP_SIZE         RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE)
#define MEMITEM_SIZE(item)      ((rt_ubase_t)item->next - (rt_ubase_t)item - RT_MEMHEAP_SIZE)
#define MEMITEM(ptr)            (struct rt_memheap_item*)((rt_uint8_t*)ptr - RT_MEMHEAP_SIZE)

#ifdef RT_USING_MEMTRACE
rt_inline void rt_memheap_setname(struct rt_memheap_item *item, const char *name)
{
    int index;
    rt_uint8_t *ptr;

    ptr = (rt_uint8_t *) & (item->next_free);
    for (index = 0; index < sizeof(void *); index ++)
    {
        if (name[index] == '\0') break;
        ptr[index] = name[index];
    }
    if (name[index] == '\0') ptr[index] = '\0';
    else
    {
        ptr = (rt_uint8_t *) & (item->prev_free);
        for (index = 0; index < sizeof(void *) && (index + sizeof(void *)) < RT_NAME_MAX; index ++)
        {
            if (name[sizeof(void *) + index] == '\0') break;
            ptr[index] = name[sizeof(void *) + index];
        }

        if (name[sizeof(void *) + index] == '\0') ptr[index] = '\0';
    }
}

void rt_mem_set_tag(void *ptr, const char *name)
{
    struct rt_memheap_item *item;

    if (ptr && name)
    {
        item = MEMITEM(ptr);
        rt_memheap_setname(item, name);
    }
}
#endif

/*
 * The initialized memory pool will be:
 * +-----------------------------------+--------------------------+
 * | whole freed memory block          | Used Memory Block Tailer |
 * +-----------------------------------+--------------------------+
 *
 * block_list --> whole freed memory block
 *
 * The length of Used Memory Block Tailer is 0,
 * which is prevents block merging across list
 */
rt_err_t rt_memheap_init(struct rt_memheap *memheap,
                         const char        *name,
                         void              *start_addr,
                         rt_size_t         size)
{
    struct rt_memheap_item *item;

    RT_ASSERT(memheap != RT_NULL);

    /* initialize pool object */
    rt_object_init(&(memheap->parent), RT_Object_Class_MemHeap, name);

    memheap->start_addr     = start_addr;
    memheap->pool_size      = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);
    memheap->available_size = memheap->pool_size - (2 * RT_MEMHEAP_SIZE);
    memheap->max_used_size  = memheap->pool_size - memheap->available_size;

    /* initialize the free list header */
    item            = &(memheap->free_header);
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    item->pool_ptr  = memheap;
    item->next      = RT_NULL;
    item->prev      = RT_NULL;
    item->next_free = item;
    item->prev_free = item;

    /* set the free list to free list header */
    memheap->free_list = item;

    /* initialize the first big memory block */
    item            = (struct rt_memheap_item *)start_addr;
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    item->pool_ptr  = memheap;
    item->next      = RT_NULL;
    item->prev      = RT_NULL;
    item->next_free = item;
    item->prev_free = item;

#ifdef RT_USING_MEMTRACE
    rt_memset(item->owner_thread_name, ' ', sizeof(item->owner_thread_name));
#endif

    item->next = (struct rt_memheap_item *)
                 ((rt_uint8_t *)item + memheap->available_size + RT_MEMHEAP_SIZE);
    item->prev = item->next;

    /* block list header */
    memheap->block_list = item;

    /* place the big memory block to free list */
    item->next_free = memheap->free_list->next_free;
    item->prev_free = memheap->free_list;
    memheap->free_list->next_free->prev_free = item;
    memheap->free_list->next_free            = item;

    /* move to the end of memory pool to build a small tailer block,
     * which prevents block merging
     */
    item = item->next;
    /* it's a used memory block */
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED);
    item->pool_ptr  = memheap;
    item->next      = (struct rt_memheap_item *)start_addr;
    item->prev      = (struct rt_memheap_item *)start_addr;
    /* not in free list */
    item->next_free = item->prev_free = RT_NULL;

    /* initialize semaphore lock */
    rt_sem_init(&(memheap->lock), name, 1, RT_IPC_FLAG_FIFO);

    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                 ("memory heap: start addr 0x%08x, size %d, free list header 0x%08x\n",
                  start_addr, size, &(memheap->free_header)));

    return RT_EOK;
}
RTM_EXPORT(rt_memheap_init);

rt_err_t rt_memheap_detach(struct rt_memheap *heap)
{
    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);
    RT_ASSERT(rt_object_is_systemobject(&heap->parent));

    rt_sem_detach(&heap->lock);
    rt_object_detach(&(heap->parent));

    /* Return a successful completion. */
    return RT_EOK;
}
RTM_EXPORT(rt_memheap_detach);

void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size)
{
    rt_err_t result;
    rt_uint32_t free_size;
    struct rt_memheap_item *header_ptr;

    RT_ASSERT(heap != RT_NULL);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    /* align allocated size */
    size = RT_ALIGN(size, RT_ALIGN_SIZE);
    if (size < RT_MEMHEAP_MINIALLOC)
        size = RT_MEMHEAP_MINIALLOC;

    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("allocate %d on heap:%8.*s",
                                    size, RT_NAME_MAX, heap->parent.name));

    if (size < heap->available_size)
    {
        /* search on free list */
        free_size = 0;

        /* lock memheap */
        result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            rt_set_errno(result);

            return RT_NULL;
        }

        /* get the first free memory block */
        header_ptr = heap->free_list->next_free;
        while (header_ptr != heap->free_list && free_size < size)
        {
            /* get current freed memory block size */
            free_size = MEMITEM_SIZE(header_ptr);
            if (free_size < size)
            {
                /* move to next free memory block */
                header_ptr = header_ptr->next_free;
            }
        }

        /* determine if the memory is available. */
        if (free_size >= size)
        {
            /* a block that satisfies the request has been found. */

            /* determine if the block needs to be split. */
            if (free_size >= (size + RT_MEMHEAP_SIZE + RT_MEMHEAP_MINIALLOC))
            {
                struct rt_memheap_item *new_ptr;

                /* split the block. */
                new_ptr = (struct rt_memheap_item *)
                          (((rt_uint8_t *)header_ptr) + size + RT_MEMHEAP_SIZE);

                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                             ("split: block[0x%08x] nextm[0x%08x] prevm[0x%08x] to new[0x%08x]\n",
                              header_ptr,
                              header_ptr->next,
                              header_ptr->prev,
                              new_ptr));

                /* mark the new block as a memory block and freed. */
                new_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);

                /* put the pool pointer into the new block. */
                new_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
                rt_memset(new_ptr->owner_thread_name, ' ', sizeof(new_ptr->owner_thread_name));
#endif

                /* break down the block list */
                new_ptr->prev          = header_ptr;
                new_ptr->next          = header_ptr->next;
                header_ptr->next->prev = new_ptr;
                header_ptr->next       = new_ptr;

                /* remove header ptr from free list */
                header_ptr->next_free->prev_free = header_ptr->prev_free;
                header_ptr->prev_free->next_free = header_ptr->next_free;
                header_ptr->next_free = RT_NULL;
                header_ptr->prev_free = RT_NULL;

                /* insert new_ptr to free list */
                new_ptr->next_free = heap->free_list->next_free;
                new_ptr->prev_free = heap->free_list;
                heap->free_list->next_free->prev_free = new_ptr;
                heap->free_list->next_free            = new_ptr;
                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("new ptr: next_free 0x%08x, prev_free 0x%08x\n",
                                                new_ptr->next_free,
                                                new_ptr->prev_free));

                /* decrement the available byte count.  */
                heap->available_size = heap->available_size -
                                       size -
                                       RT_MEMHEAP_SIZE;
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;
            }
            else
            {
                /* decrement the entire free size from the available bytes count. */
                heap->available_size = heap->available_size - free_size;
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;

                /* remove header_ptr from free list */
                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                             ("one block: block[0x%08x], next_free 0x%08x, prev_free 0x%08x\n",
                              header_ptr,
                              header_ptr->next_free,
                              header_ptr->prev_free));

                header_ptr->next_free->prev_free = header_ptr->prev_free;
                header_ptr->prev_free->next_free = header_ptr->next_free;
                header_ptr->next_free = RT_NULL;
                header_ptr->prev_free = RT_NULL;
            }

            /* Mark the allocated block as not available. */
            header_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED);

#ifdef RT_USING_MEMTRACE
            if (rt_thread_self())
                rt_memcpy(header_ptr->owner_thread_name, rt_thread_self()->name, sizeof(header_ptr->owner_thread_name));
            else
                rt_memcpy(header_ptr->owner_thread_name, "NONE", sizeof(header_ptr->owner_thread_name));
#endif

            /* release lock */
            rt_sem_release(&(heap->lock));

            /* Return a memory address to the caller.  */
            RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                         ("alloc mem: memory[0x%08x], heap[0x%08x], size: %d\n",
                          (void *)((rt_uint8_t *)header_ptr + RT_MEMHEAP_SIZE),
                          header_ptr,
                          size));

            return (void *)((rt_uint8_t *)header_ptr + RT_MEMHEAP_SIZE);
        }

        /* release lock */
        rt_sem_release(&(heap->lock));
    }

    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("allocate memory: failed\n"));

    /* Return the completion status.  */
    return RT_NULL;
}
RTM_EXPORT(rt_memheap_alloc);

void *rt_memheap_realloc(struct rt_memheap *heap, void *ptr, rt_size_t newsize)
{
    rt_err_t result;
    rt_size_t oldsize;
    struct rt_memheap_item *header_ptr;
    struct rt_memheap_item *new_ptr;

    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    if (newsize == 0)
    {
        rt_memheap_free(ptr);

        return RT_NULL;
    }
    /* align allocated size */
    newsize = RT_ALIGN(newsize, RT_ALIGN_SIZE);
    if (newsize < RT_MEMHEAP_MINIALLOC)
        newsize = RT_MEMHEAP_MINIALLOC;

    if (ptr == RT_NULL)
    {
        return rt_memheap_alloc(heap, newsize);
    }

    /* get memory block header and get the size of memory block */
    header_ptr = (struct rt_memheap_item *)
                 ((rt_uint8_t *)ptr - RT_MEMHEAP_SIZE);
    oldsize = MEMITEM_SIZE(header_ptr);
    /* re-allocate memory */
    if (newsize > oldsize)
    {
        void *new_ptr;
        /* Fix the crash problem after opening Oz optimization on ac6  */
        volatile struct rt_memheap_item *next_ptr;

        /* lock memheap */
        result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            rt_set_errno(result);
            return RT_NULL;
        }

        next_ptr = header_ptr->next;

        /* header_ptr should not be the tail */
        RT_ASSERT(next_ptr > header_ptr);

        /* check whether the following free space is enough to expand */
        if (!RT_MEMHEAP_IS_USED(next_ptr))
        {
            rt_int32_t nextsize;

            nextsize = MEMITEM_SIZE(next_ptr);
            RT_ASSERT(next_ptr > 0);

            /* Here is the ASCII art of the situation that we can make use of
             * the next free node without alloc/memcpy, |*| is the control
             * block:
             *
             *      oldsize           free node
             * |*|-----------|*|----------------------|*|
             *         newsize          >= minialloc
             * |*|----------------|*|-----------------|*|
             */
            if (nextsize + oldsize > newsize + RT_MEMHEAP_MINIALLOC)
            {
                /* decrement the entire free size from the available bytes count. */
                heap->available_size = heap->available_size - (newsize - oldsize);
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;

                /* remove next_ptr from free list */
                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                             ("remove block: block[0x%08x], next_free 0x%08x, prev_free 0x%08x",
                              next_ptr,
                              next_ptr->next_free,
                              next_ptr->prev_free));

                next_ptr->next_free->prev_free = next_ptr->prev_free;
                next_ptr->prev_free->next_free = next_ptr->next_free;
                next_ptr->next->prev = next_ptr->prev;
                next_ptr->prev->next = next_ptr->next;

                /* build a new one on the right place */
                next_ptr = (struct rt_memheap_item *)((char *)ptr + newsize);

                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                             ("new free block: block[0x%08x] nextm[0x%08x] prevm[0x%08x]",
                              next_ptr,
                              next_ptr->next,
                              next_ptr->prev));

                /* mark the new block as a memory block and freed. */
                next_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);

                /* put the pool pointer into the new block. */
                next_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
                rt_memset(next_ptr->owner_thread_name, ' ', sizeof(next_ptr->owner_thread_name));
#endif

                next_ptr->prev          = header_ptr;
                next_ptr->next          = header_ptr->next;
                header_ptr->next->prev = (struct rt_memheap_item *)next_ptr;
                header_ptr->next       = (struct rt_memheap_item *)next_ptr;

                /* insert next_ptr to free list */
                next_ptr->next_free = heap->free_list->next_free;
                next_ptr->prev_free = heap->free_list;
                heap->free_list->next_free->prev_free = (struct rt_memheap_item *)next_ptr;
                heap->free_list->next_free            = (struct rt_memheap_item *)next_ptr;
                RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("new ptr: next_free 0x%08x, prev_free 0x%08x",
                                                next_ptr->next_free,
                                                next_ptr->prev_free));

                /* release lock */
                rt_sem_release(&(heap->lock));

                return ptr;
            }
        }

        /* release lock */
        rt_sem_release(&(heap->lock));

        /* re-allocate a memory block */
        new_ptr = (void *)rt_memheap_alloc(heap, newsize);
        if (new_ptr != RT_NULL)
        {
            rt_memcpy(new_ptr, ptr, oldsize < newsize ? oldsize : newsize);
            rt_memheap_free(ptr);
        }

        return new_ptr;
    }

    /* don't split when there is less than one node space left */
    if (newsize + RT_MEMHEAP_SIZE + RT_MEMHEAP_MINIALLOC >= oldsize)
        return ptr;

    /* lock memheap */
    result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_set_errno(result);

        return RT_NULL;
    }

    /* split the block. */
    new_ptr = (struct rt_memheap_item *)
              (((rt_uint8_t *)header_ptr) + newsize + RT_MEMHEAP_SIZE);

    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                 ("split: block[0x%08x] nextm[0x%08x] prevm[0x%08x] to new[0x%08x]\n",
                  header_ptr,
                  header_ptr->next,
                  header_ptr->prev,
                  new_ptr));

    /* mark the new block as a memory block and freed. */
    new_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    /* put the pool pointer into the new block. */
    new_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
    rt_memset(new_ptr->owner_thread_name, ' ', sizeof(new_ptr->owner_thread_name));
#endif

    /* break down the block list */
    new_ptr->prev          = header_ptr;
    new_ptr->next          = header_ptr->next;
    header_ptr->next->prev = new_ptr;
    header_ptr->next       = new_ptr;

    /* determine if the block can be merged with the next neighbor. */
    if (!RT_MEMHEAP_IS_USED(new_ptr->next))
    {
        struct rt_memheap_item *free_ptr;

        /* merge block with next neighbor. */
        free_ptr = new_ptr->next;
        heap->available_size = heap->available_size - MEMITEM_SIZE(free_ptr);

        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                     ("merge: right node 0x%08x, next_free 0x%08x, prev_free 0x%08x\n",
                      header_ptr, header_ptr->next_free, header_ptr->prev_free));

        free_ptr->next->prev = new_ptr;
        new_ptr->next   = free_ptr->next;

        /* remove free ptr from free list */
        free_ptr->next_free->prev_free = free_ptr->prev_free;
        free_ptr->prev_free->next_free = free_ptr->next_free;
    }

    /* insert the split block to free list */
    new_ptr->next_free = heap->free_list->next_free;
    new_ptr->prev_free = heap->free_list;
    heap->free_list->next_free->prev_free = new_ptr;
    heap->free_list->next_free            = new_ptr;
    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("new free ptr: next_free 0x%08x, prev_free 0x%08x\n",
                                    new_ptr->next_free,
                                    new_ptr->prev_free));

    /* increment the available byte count.  */
    heap->available_size = heap->available_size + MEMITEM_SIZE(new_ptr);

    /* release lock */
    rt_sem_release(&(heap->lock));

    /* return the old memory block */
    return ptr;
}
RTM_EXPORT(rt_memheap_realloc);

void rt_memheap_free(void *ptr)
{
    rt_err_t result;
    struct rt_memheap *heap;
    struct rt_memheap_item *header_ptr, *new_ptr;
    rt_uint32_t insert_header;

    /* NULL check */
    if (ptr == RT_NULL) return;

    /* set initial status as OK */
    insert_header = 1;
    new_ptr       = RT_NULL;
    header_ptr    = (struct rt_memheap_item *)
                    ((rt_uint8_t *)ptr - RT_MEMHEAP_SIZE);

    RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("free memory: memory[0x%08x], block[0x%08x]\n",
                                    ptr, header_ptr));

    /* check magic */
    if (header_ptr->magic != (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED))
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("bad magic:0x%08x @ memheap\n",
                                        header_ptr->magic));
    }
    RT_ASSERT(header_ptr->magic == (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED));
    /* check whether this block of memory has been over-written. */
    RT_ASSERT((header_ptr->next->magic & RT_MEMHEAP_MASK) == RT_MEMHEAP_MAGIC);

    /* get pool ptr */
    heap = header_ptr->pool_ptr;

    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    /* lock memheap */
    result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
    if (result != RT_EOK)
    {
        rt_set_errno(result);

        return ;
    }

    /* Mark the memory as available. */
    header_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    /* Adjust the available number of bytes. */
    heap->available_size += MEMITEM_SIZE(header_ptr);

    /* Determine if the block can be merged with the previous neighbor. */
    if (!RT_MEMHEAP_IS_USED(header_ptr->prev))
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("merge: left node 0x%08x\n",
                                        header_ptr->prev));

        /* adjust the available number of bytes. */
        heap->available_size += RT_MEMHEAP_SIZE;

        /* yes, merge block with previous neighbor. */
        (header_ptr->prev)->next = header_ptr->next;
        (header_ptr->next)->prev = header_ptr->prev;

        /* move header pointer to previous. */
        header_ptr = header_ptr->prev;
        /* don't insert header to free list */
        insert_header = 0;
    }

    /* determine if the block can be merged with the next neighbor. */
    if (!RT_MEMHEAP_IS_USED(header_ptr->next))
    {
        /* adjust the available number of bytes. */
        heap->available_size += RT_MEMHEAP_SIZE;

        /* merge block with next neighbor. */
        new_ptr = header_ptr->next;

        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                     ("merge: right node 0x%08x, next_free 0x%08x, prev_free 0x%08x\n",
                      new_ptr, new_ptr->next_free, new_ptr->prev_free));

        new_ptr->next->prev = header_ptr;
        header_ptr->next    = new_ptr->next;

        /* remove new ptr from free list */
        new_ptr->next_free->prev_free = new_ptr->prev_free;
        new_ptr->prev_free->next_free = new_ptr->next_free;
    }

    if (insert_header)
    {
        /* no left merge, insert to free list */
        header_ptr->next_free = heap->free_list->next_free;
        header_ptr->prev_free = heap->free_list;
        heap->free_list->next_free->prev_free = header_ptr;
        heap->free_list->next_free            = header_ptr;

        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP,
                     ("insert to free list: next_free 0x%08x, prev_free 0x%08x\n",
                      header_ptr->next_free, header_ptr->prev_free));
    }

#ifdef RT_USING_MEMTRACE
    rt_memset(header_ptr->owner_thread_name, ' ', sizeof(header_ptr->owner_thread_name));
#endif

    /* release lock */
    rt_sem_release(&(heap->lock));
}
RTM_EXPORT(rt_memheap_free);

#ifdef RT_USING_FINSH
static void _memheap_dump_tag(struct rt_memheap_item *item)
{
    rt_uint8_t name[2 * sizeof(void *)];
    rt_uint8_t *ptr;

    ptr = (rt_uint8_t *) & (item->next_free);
    rt_memcpy(name, ptr, sizeof(void *));
    ptr = (rt_uint8_t *) & (item->prev_free);
    rt_memcpy(&name[sizeof(void *)], ptr, sizeof(void *));

    rt_kprintf("%.*s", 2 * sizeof(void *), name);
}

int rt_memheap_dump(struct rt_memheap *heap)
{
    struct rt_memheap_item *item, *end;

    if (heap == RT_NULL) return 0;
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    rt_kprintf("\n[%.*s] [0x%08x - 0x%08x]->\n", RT_NAME_MAX, heap->parent.name,
               (rt_ubase_t)heap->start_addr, (rt_ubase_t)heap->start_addr + heap->pool_size);
    rt_kprintf("------------------------------\n");

    /* lock memheap */
    rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
    item = heap->block_list;

    end = (struct rt_memheap_item *)((rt_uint8_t *)heap->start_addr + heap->pool_size - RT_MEMHEAP_SIZE);

    /* for each memory block */
    while ((rt_ubase_t)item < ((rt_ubase_t)end))
    {
        if (RT_MEMHEAP_IS_USED(item) && ((item->magic & RT_MEMHEAP_MASK) != RT_MEMHEAP_MAGIC))
            rt_kprintf("0x%08x", item + 1);

        if (item->magic == (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED))
        {
            rt_kprintf("0x%08x: %-8d ",     item + 1, MEMITEM_SIZE(item));
            _memheap_dump_tag(item);
            rt_kprintf("\n");
        }
        else
        {
            rt_kprintf("0x%08x: %-8d <F>\n", item + 1, MEMITEM_SIZE(item));
        }

        item = item->next;
    }
    rt_sem_release(&(heap->lock));

    return 0;
}

int memheaptrace(void)
{
    int count = rt_object_get_length(RT_Object_Class_MemHeap);
    struct rt_memheap **heaps;

    if (count > 0)
    {
        int index;
        extern int list_memheap(void);

        heaps = (struct rt_memheap **)rt_malloc(sizeof(struct rt_memheap *) * count);
        if (heaps == RT_NULL) return 0;

        list_memheap();

        rt_kprintf("memheap header size: %d\n", RT_MEMHEAP_SIZE);
        count = rt_object_get_pointers(RT_Object_Class_MemHeap, (rt_object_t *)heaps, count);
        for (index = 0; index < count; index++)
        {
            rt_memheap_dump(heaps[index]);
        }

        rt_free(heaps);
    }

    return 0;
}
MSH_CMD_EXPORT(memheaptrace, dump memory trace information);
#endif

#ifdef RT_USING_MEMHEAP_AS_HEAP
static struct rt_memheap _heap;

void rt_system_heap_init(void *begin_addr, void *end_addr)
{
    /* initialize a default heap in the system */
    rt_memheap_init(&_heap,
                    "heap",
                    begin_addr,
                    (rt_uint32_t)end_addr - (rt_uint32_t)begin_addr);
}

void *rt_malloc(rt_size_t size)
{
    void *ptr;

    /* try to allocate in system heap */
    ptr = rt_memheap_alloc(&_heap, size);
    if (ptr == RT_NULL)
    {
        struct rt_object *object;
        struct rt_list_node *node;
        struct rt_memheap *heap;
        struct rt_object_information *information;

        /* try to allocate on other memory heap */
        information = rt_object_get_information(RT_Object_Class_MemHeap);
        RT_ASSERT(information != RT_NULL);
        for (node  = information->object_list.next;
             node != &(information->object_list);
             node  = node->next)
        {
            object = rt_list_entry(node, struct rt_object, list);
            heap   = (struct rt_memheap *)object;

            RT_ASSERT(heap);
            RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

            /* not allocate in the default system heap */
            if (heap == &_heap)
                continue;

            ptr = rt_memheap_alloc(heap, size);
            if (ptr != RT_NULL)
                break;
        }
    }


#ifdef RT_USING_MEMTRACE
    if (ptr == RT_NULL)
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("malloc[%d] => NULL", size));
    }
    else
    {
        struct rt_memheap_item *item = MEMITEM(ptr);
        if (rt_thread_self())
            rt_memheap_setname(item, rt_thread_self()->name);
        else
            rt_memheap_setname(item, "<null>");

        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("malloc => 0x%08x : %d", ptr, size));
    }
#endif

    return ptr;
}
RTM_EXPORT(rt_malloc);

void rt_free(void *rmem)
{
    rt_memheap_free(rmem);
}
RTM_EXPORT(rt_free);

void *rt_realloc(void *rmem, rt_size_t newsize)
{
    void *new_ptr;
    struct rt_memheap_item *header_ptr;

    if (rmem == RT_NULL)
        return rt_malloc(newsize);

    if (newsize == 0)
    {
        rt_free(rmem);
        return RT_NULL;
    }

    /* get old memory item */
    header_ptr = (struct rt_memheap_item *)
                 ((rt_uint8_t *)rmem - RT_MEMHEAP_SIZE);

    new_ptr = rt_memheap_realloc(header_ptr->pool_ptr, rmem, newsize);
    if (new_ptr == RT_NULL && newsize != 0)
    {
        /* allocate memory block from other memheap */
        new_ptr = rt_malloc(newsize);
        if (new_ptr != RT_NULL && rmem != RT_NULL)
        {
            rt_size_t oldsize;

            /* get the size of old memory block */
            oldsize = MEMITEM_SIZE(header_ptr);
            if (newsize > oldsize)
                rt_memcpy(new_ptr, rmem, oldsize);
            else
                rt_memcpy(new_ptr, rmem, newsize);

            rt_free(rmem);
        }
    }

#ifdef RT_USING_MEMTRACE
    if (new_ptr == RT_NULL)
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("realloc[%d] => NULL", newsize));
    }
    else
    {
        struct rt_memheap_item *item = MEMITEM(new_ptr);
        if (rt_thread_self())
            rt_memheap_setname(item, rt_thread_self()->name);
        else
            rt_memheap_setname(item, "<null>");

        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("realloc => 0x%08x : %d",
                                        new_ptr, newsize));
    }
#endif

    return new_ptr;
}
RTM_EXPORT(rt_realloc);

void *rt_calloc(rt_size_t count, rt_size_t size)
{
    void *ptr;
    rt_size_t total_size;

    total_size = count * size;
    ptr = rt_malloc(total_size);
    if (ptr != RT_NULL)
    {
        /* clean memory */
        rt_memset(ptr, 0, total_size);
    }

#ifdef RT_USING_MEMTRACE
    if (ptr == RT_NULL)
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("calloc[%d x %d] => NULL",
                                        count, size));
    }
    else
    {
        RT_DEBUG_LOG(RT_DEBUG_MEMHEAP, ("calloc => 0x%08x : %d",
                                        ptr, count * size));
    }
#endif

    return ptr;
}
RTM_EXPORT(rt_calloc);

void rt_memory_info(rt_uint32_t *total,
                    rt_uint32_t *used,
                    rt_uint32_t *max_used)
{
    if (total != RT_NULL)
        *total = _heap.pool_size;

    if (used  != RT_NULL)
        *used = _heap.pool_size - _heap.available_size;

    if (max_used != RT_NULL)
        *max_used = _heap.max_used_size;
}

#endif

#ifdef RT_USING_MEMTRACE

void dump_used_memheap(struct rt_memheap *mh)
{
    struct rt_memheap_item *header_ptr;
    rt_uint32_t block_size;


    rt_kprintf("\nmemory heap address:\n");
    rt_kprintf("heap_ptr: 0x%08x\n", mh->start_addr);
    rt_kprintf("free    : 0x%08x\n", mh->available_size);
    rt_kprintf("max_used: 0x%08x\n", mh->max_used_size);
    rt_kprintf("size    : 0x%08x\n", mh->pool_size);

    rt_kprintf("\n--memory used information --\n");

    header_ptr = mh->block_list;
    while (header_ptr->next != mh->block_list)
    {
        if ((header_ptr->magic & RT_MEMHEAP_MASK) != RT_MEMHEAP_MAGIC)
        {
            rt_kprintf("[0x%08x - incorrect magic: 0x%08x\n", header_ptr, header_ptr->magic);
            break;
        }

        /* get current memory block size */
        block_size = MEMITEM_SIZE(header_ptr);
        if (block_size < 0)
            break;

        if (RT_MEMHEAP_IS_USED(header_ptr))
        {
            /* dump information */
            rt_kprintf("[0x%08x - %d - %c%c%c%c] used\n", header_ptr, block_size,
                       header_ptr->owner_thread_name[0], header_ptr->owner_thread_name[1],
                       header_ptr->owner_thread_name[2], header_ptr->owner_thread_name[3]);
        }
        else
        {
            /* dump information */
            rt_kprintf("[0x%08x - %d - %c%c%c%c] free\n", header_ptr, block_size,
                       header_ptr->owner_thread_name[0], header_ptr->owner_thread_name[1],
                       header_ptr->owner_thread_name[2], header_ptr->owner_thread_name[3]);
        }

        /* move to next used memory block */
        header_ptr = header_ptr->next;
    }
}

void memtrace_heap()
{
    struct rt_object_information *info;
    struct rt_list_node *list;
    struct rt_memheap *mh;
    struct rt_list_node *node;

    info = rt_object_get_information(RT_Object_Class_MemHeap);
    list = &info->object_list;

    for (node = list->next; node != list; node = node->next)
    {
        mh = (struct rt_memheap *)rt_list_entry(node, struct rt_object, list);
        dump_used_memheap(mh);
    }
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(memtrace_heap, dump memory trace for heap);
#endif /* end of RT_USING_FINSH */

#endif /* end of RT_USING_MEMTRACE */

#endif /* end of RT_USING_MEMHEAP */
