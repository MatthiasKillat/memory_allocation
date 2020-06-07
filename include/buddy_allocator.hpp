#pragma once

#include <sys/mman.h>
#include <assert.h>
#include <iostream>

//note: prototype code, must be cleaned up, optimzed and tested  but seems to work conceptually :-)

//todo: alignment issues, lockfree/concurrency, memory/lookup optimization ...

//note that the indexing is (seemingly) complicated due to the effort in trying to save storage
//and avoiding extra memory headers before the actual payload (which is a very nice a property to have)
//nevertheless, there should be room for improvement and clarity :-)

//there is also always the possibility of an extra abstraction with a map/set/other search structure on top
//to avoid double frees/frees on pointers never allocated (inucrring a performance loss)
//the map would have to support max number of blocks AND be threadsafe/lockfree

//this does not protect against accidental writes from the user in the metadata of the allocator
//(which could be handled differently, with a larger memory overhead and might require the metadata
//to be locatable effectively (are there options besides a map or similar?))

//todo: final goal - lockfree, safe and reasonably fast allocator with high memory utilization

class BuddyAllocator
{
public:
    static constexpr uint64_t MAX_LEVELS = 5;      //reasonable limit would be 32 with the minimal block size below
    static constexpr uint64_t MIN_BLOCK_SIZE = 16; //need to store 2 pointers in free blocks
    static constexpr uint64_t MAX_MEMORY_SIZE = (1ULL << MAX_LEVELS) * MIN_BLOCK_SIZE;
    static constexpr uint64_t MAX_NUMBER_OF_BLOCKS = (1ULL << MAX_LEVELS) - 1;

private:
    using int_ptr_t = uint64_t;

    //only used in free blocks (for maximum safety it may be better to do this in an external structure)
    //but this way we safe memory (the free blocks are not used, but we can accidentially write to them if we
    //got a pointer from the allocator and write more bytes than we requested)
    struct Header
    {
        Header *prev;
        Header *next;
    };

    enum BlockStatus
    {
        Split,
        Free,
        Allocated
    };

    //memory consumption can be optimized (xor trick for buddies, bit optimization)
    struct BlockInfo
    {
        BlockStatus status{Free};
    };

    //***************************************************************

    void *m_memory;

    //can be stored differently, e.g. in the memory itself (which requires a more careful implementation)
    //the actual storage also just depends on the actually managed memory size (we assume the worst case here)
    //todo: optimize storage of metadata
    BlockInfo m_blockInfo[MAX_NUMBER_OF_BLOCKS];
    uint64_t m_maxLevel;
    uint64_t m_numBytes;

    Header *m_freeLists[MAX_LEVELS + 1]{nullptr};
    uint64_t m_levelSize[MAX_LEVELS + 1];
    uint64_t m_levelStartIndex[MAX_LEVELS + 1];

    //***************************************************************

    static void *createMemory(uint64_t size)
    {

        if (size > MAX_MEMORY_SIZE)
        {
            return nullptr;
        }

        void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_ANON | MAP_PRIVATE, 0, 0);

        if (mapped == MAP_FAILED)
        {
            return nullptr;
        }

        assert(((int_ptr_t)mapped & 0xfff) == 0); //page aligned? (assumes 4096 Byte page size)

        return mapped;
    }

    static uint64_t nextPowerOfTwo(uint64_t n)
    {
        uint64_t p = 1;
        while (p < n)
        {
            p <<= 1;
        }
        return p;
    }

    //index to pointer conversion and vice versa, sometimes level must be known in addition

    uint64_t indexInLevel(void *ptr, uint64_t level)
    {
        return ((int_ptr_t)(ptr) - (int_ptr_t)(m_memory)) / m_levelSize[level];
    }

    void *ptrInLevel(uint64_t index, uint64_t level)
    {
        return reinterpret_cast<void *>((int_ptr_t)(index * m_levelSize[level]) + (int_ptr_t)(m_memory));
    }

    uint64_t indexOf(void *ptr, uint64_t level)
    {
        return indexInLevel(ptr, level) + m_levelStartIndex[level];
    }

    //only works when the status updates are accounted for
    //(essentially only for blocks that have been allocated or blocks above those  in the tree)
    uint64_t indexOf(void *ptr)
    {
        return indexOf(ptr, levelOf(ptr));
    }

    //only works when the status updates are accounted for
    //(essentially only for blocks that have been allocated or blocks above those  in the tree)
    uint64_t levelOf(void *ptr)
    {
        auto n = m_maxLevel;
        while (n > 0)
        {
            auto index = indexOf(ptr, n - 1);
            if (m_blockInfo[index].status == Split)
            {
                return n;
            }
            --n;
        }
        return 0;
    }

    static uint64_t levelOf(uint64_t index)
    {
        //make more efficient if really needed
        ++index;
        auto level = 0;
        while (index > 1)
        {
            index >>= 1;
            ++level;
        }
        return level;
    }

    void *addressOf(uint64_t index, uint64_t level)
    {
        index -= m_levelStartIndex[level];
        return ptrInLevel(index, level);
    }

    void *addressOf(uint64_t index)
    {
        return addressOf(index, levelOf(index));
    }

    uint64_t findLevel(size_t size)
    {
        uint64_t level = m_maxLevel;
        while (size > m_levelSize[level])
        {
            --level;
        }
        return level;
    }

    //we always remove from the front, there may be potential to use a lockfree stack/queue structure
    //to obtain a lockfree allocator (at the cost of memory efficiency)
    void *removeFromListFront(uint64_t level)
    {
        auto freeList = m_freeLists[level];
        if (!freeList)
        {
            return nullptr;
        }

        auto next = freeList->next;
        if (next)
        {
            next->prev = nullptr; //do we need prev?
        }
        m_freeLists[level] = next;

        return freeList;
    }

    void removeFromList(Header *block, uint64_t level)
    {
        auto freeList = m_freeLists[level];

        //this check is more secure than just testing block->prev == nullptr (in case block was overwritten, see vulnerabilities
        //explained at the beginning)
        if (block == freeList)
        {
            //block is the first in list
            if (freeList->next)
            {
                freeList->next->prev = nullptr;
            }
            m_freeLists[level] = freeList->next;
        }
        else
        {
            //block is in the middle or at the end of the list (i.e. prev exists)
            if (block->prev)
            {
                block->prev->next = block->next;
            }
            if (block->next)
            {
                block->next->prev = block->prev;
            }
        }

        //todo: should be unnecessary, the node is unlinked - but easier to debug when the pointers are unset
        //(this only matters when a block is freed that was not allocated, and then we are in trouble anyway)
        block->next = nullptr;
        block->prev = nullptr;
    }

    //todo: do we want/need to insert in block order in the level?
    void insertToList(Header *block, uint64_t level)
    {
        auto freeList = m_freeLists[level];

        block->prev = nullptr;

        if (freeList)
        {
            freeList->prev = block;
        }

        block->next = freeList;
        m_freeLists[level] = block;
    }

    uint64_t split(const uint64_t index, uint64_t level)
    {
        m_blockInfo[index].status = Split;

        auto buddyIndex = right(index);

        //the split buddy is marked free and added to the freelist
        m_blockInfo[buddyIndex].status = Free;

        Header *buddy = reinterpret_cast<Header *>(addressOf(buddyIndex));

        insertToList(buddy, level + 1);

        return left(index);
    }

    static uint64_t parent(uint64_t index)
    {
        return index / 2 - (index + 1) % 2;
    }

    static uint64_t left(uint64_t index)
    {
        return 2 * index + 1;
    }

    static uint64_t right(uint64_t index)
    {
        return 2 * index + 2;
    }

    uint64_t buddy(uint64_t index)
    {
        if (index % 2 == 0)
        {
            return index - 1;
        }
        return index + 1;
    }

    //***************************************************************
    //debug
    //***************************************************************

public:
    char status(uint64_t index)
    {
        if (m_blockInfo[index].status == Split)
        {
            return 'S';
        }
        else if (m_blockInfo[index].status == Free)
        {
            return 'F';
        }
        else
        {
            return 'A';
        }
    }

    void printTree()
    {
        uint64_t level = 0;
        uint64_t nextLevelIndex = 0;
        uint64_t index = 0;
        std::cout << "block tree ";
        //bad logic, but will be removed after debugging is done
        while (level <= m_maxLevel)
        {
            std::cout << std::endl
                      << "level " << level << " blocksize " << m_levelSize[level] << ": ";
            nextLevelIndex = (nextLevelIndex + 1) * 2 - 1;

            //print the level
            while (index < nextLevelIndex)
            {

                auto p = parent(index);

                std::cout << index;
                if (index == 0 || m_blockInfo[p].status == Split)
                {
                    std::cout << status(index);
                }

                std::cout << " ";
                ++index;
            }

            //next level
            ++level;
        }
        std::cout << std::endl;
    }

    void printFreeList()
    {
        std::cout << "free lists ";
        for (uint64_t level = 0; level <= m_maxLevel; ++level)
        {
            auto freeList = m_freeLists[level];
            std::cout << std::endl
                      << "level " << level << " blocksize " << m_levelSize[level] << ": ";

            while (freeList)
            {
                auto index = indexOf(freeList);
                std::cout << index;

                if (freeList->prev)
                {
                    std::cout << "(" << indexOf(freeList->prev) << ")";
                }

                //std::cout << " " << addressOf(index) << " ";
                std::cout << " ";

                freeList = freeList->next;
            }
        }
        std::cout << std::endl;
    }

    void printBlockAddresses()
    {
        uint64_t level = 0;
        uint64_t nextLevelIndex = 0;
        uint64_t index = 0;
        std::cout << "block addresses ";
        while (level <= m_maxLevel)
        {
            std::cout << std::endl
                      << "level " << level << " blocksize " << m_levelSize[level] << ": ";
            nextLevelIndex = (nextLevelIndex + 1) * 2 - 1;

            //print the level
            while (index < nextLevelIndex)
            {
                auto ptr = addressOf(index);
                std::cout << index << ": " << ptr << " ";
                ++index;
            }

            //next level
            ++level;
        }
        std::cout << std::endl;
    }

    void print()
    {
        static uint64_t count = 0;
        std::cout << "*****************allocator state " << count++ << " ************" << std::endl;
        printTree();
        printFreeList();
        std::cout << "***********************************************" << std::endl;
    }

    //***************************************************************
    //construction
    //***************************************************************

    BuddyAllocator(uint64_t requestedSize)
    {
        //create memory

        //round up to next power of two
        //technically we can use a trick to use memory that is not a power of 2 (marking unavailable blocks as initially not free)
        m_numBytes = nextPowerOfTwo(requestedSize);

        if (m_numBytes < MIN_BLOCK_SIZE)
        {
            m_numBytes = MIN_BLOCK_SIZE;
        }

        //todo: deal with failure
        m_memory = createMemory(m_numBytes);

        //initialize parts of index structure

        auto size = m_numBytes;
        m_maxLevel = 0;
        m_levelSize[0] = size;
        m_levelStartIndex[0] = 0;
        uint64_t index = 0;

        while (size >= MIN_BLOCK_SIZE)
        {
            m_levelSize[m_maxLevel] = size;

            m_levelStartIndex[m_maxLevel] = index; //compute the starting index for each level
            index = (index + 1) * 2 - 1;

            ++m_maxLevel;
            size >>= 1;
        }
        --m_maxLevel;

        //setup freelists
        Header *block = reinterpret_cast<Header *>(m_memory);
        block->next = nullptr;
        block->prev = nullptr;

        m_freeLists[0] = block;
        //all other freelists are empty initially
    }

    BuddyAllocator(const BuddyAllocator &) = delete;
    BuddyAllocator(BuddyAllocator &&) = delete;

    //***************************************************************
    //public API
    //***************************************************************

    void *allocate(size_t requestedBytes)
    {
        if (requestedBytes == 0)
        {
            std::cout << "###requested 0 bytes - returning nullptr" << std::endl;
            return nullptr;
        }

        if (requestedBytes > m_numBytes)
        {
            std::cout << "###requested more bytes than the allocator manages - returning nullptr" << std::endl;
            return nullptr;
        }

        auto requiredLevel = findLevel(requestedBytes);

        std::cout << "###requested " << requestedBytes << " bytes required level " << requiredLevel << std::endl;

        //find the best fitting non-empty free list
        uint64_t level = requiredLevel;

        auto freeList = m_freeLists[level];

        while (!freeList)
        {
            if (level == 0)
            {
                std::cout << "###no sufficently large block available - returning nullptr " << std::endl;
                return nullptr; //no sufficiently large block available
            }

            //try next freelist
            freeList = m_freeLists[--level];
        }

        //get the first free block from that list (todo: not necessarily in memory layout order!, is this needed?)
        auto block = removeFromListFront(level);

        //may need to split the block to avoid wasting large ammounts of memory
        //however, small waste (internal fragmentation) is unavoidable with this allocator
        //todo: combine it with another allocator to deal with this (which would take hover the rest of the partially wasted block)

        auto index = indexOf(block);

        while (level != requiredLevel)
        {
            index = split(index, level);
            ++level;
        }

        //mark block as allocated
        m_blockInfo[index].status = Allocated;

        std::cout << "###returning block " << indexOf(block) << " address " << block << std::endl;

        return block;
    }

    void free(void *block)
    {
        //todo: sanity checks if desired
        //could check whether the pointer is valid in the sense that it is a "multiple of powers of 2 relative to start"
        //(this does not guarantee it was allocated before)

        if (block == nullptr)
        {
            std::cout << "### freeing nullptr - returning" << std::endl;
            return;
        }

        //assume the returned block is valid

        auto index = indexOf(block); //works only if the structure was not corrupted and the block was allocated before

        std::cout << "### freeing block " << index << " with address " << block << std::endl;

        if (index == 0)
        {
            std::cout << "### freeing root block " << index << " with address " << block << std::endl;
            //return root block into freelist, i.e. no other blocks are allocated
            insertToList(reinterpret_cast<Header *>(block), 0);
            m_blockInfo[index].status == Free;

            return;
        }

        m_blockInfo[index].status = Free;

        auto buddyIndex = buddy(index);
        auto level = levelOf(index); //is > 0, otherwise we would have freed the root block above

        //merge blocks upwards if the buddy is free
        //todo: logic can probably be optimized
        while (m_blockInfo[buddyIndex].status == Free)
        {
            Header *buddyAddress = reinterpret_cast<Header *>(addressOf(buddyIndex));

            // std::cout << "### merging index " << index << " with buddy " << buddyIndex
            //           << " with address " << buddyAddress << " and prev " << buddyAddress->prev << std::endl;

            //remove buddy from its freelist (this is why we need the prev pointer)
            removeFromList(buddyAddress, level);

            //merged index and its buddy into parent block
            index = parent(index);
            m_blockInfo[index].status = Free; //was split, is now free

            --level;

            //do we continue merging?
            if (level == 0)
            {
                break; // we arrived at the root, no further merging
            }
            buddyIndex = buddy(index);
        }

        //insert the final merged block into the freelist
        Header *indexAddress = reinterpret_cast<Header *>(addressOf(index));
        insertToList(indexAddress, level);
    }
};
