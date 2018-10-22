#include "NullDevice.h"
#include "Limits.h"
#include <AK/StdLib.h>
#include <AK/kstdio.h>

NullDevice::NullDevice()
{
}

NullDevice::~NullDevice()
{
}

Unix::ssize_t NullDevice::read(byte*, Unix::size_t)
{
    kprintf("NullDevice: read from null\n");
    return 0;
}

Unix::ssize_t NullDevice::write(const byte*, Unix::size_t bufferSize)
{
    return min(GoodBufferSize, bufferSize);
}
