#include <iostream>

#include "buddy_allocator.hpp"

int main(int argc, char **argv)
{
    BuddyAllocator allocator(200); //will round up to 256 Bytes ... buddies, the power of two, makes sense:-)

    //std::cout << "MAX NUMBER OF BLOCKS " << BuddyAllocator::MAX_NUMBER_OF_BLOCKS << std::endl;

    allocator.printBlockAddresses();
    allocator.print();

    auto block1 = allocator.allocate(28);

    allocator.print();

    auto block2 = allocator.allocate(33);

    allocator.print();

    auto block3 = allocator.allocate(64);

    allocator.print();

    allocator.free(block1);

    allocator.print();

    auto block4 = allocator.allocate(120); //should fail

    allocator.print();

    allocator.free(block3);

    allocator.print();

    allocator.free(block4); //is a nullptr, nothing happens

    allocator.print();

    allocator.free(block2);

    allocator.print(); //allocator memory is completely free again

    //we have 16 16 bit blocks available, 16*16 = 256 ) total memory of the allocator
    constexpr int n = 16;
    void *blocks[n];

    for (int i = 0; i < n; ++i)
    {
        blocks[i] = allocator.allocate(15);
    }

    allocator.print();

    for (int i = 0; i < n; i += 2)
    {
        allocator.free(blocks[i]);
    }

    allocator.print();

    for (int i = 1; i < n; i += 4)
    {
        allocator.free(blocks[i]);
    }

    allocator.print();

    for (int i = 3; i < n; i += 4)
    {
        allocator.free(blocks[i]);
    }

    allocator.print();

    return 0;
}
