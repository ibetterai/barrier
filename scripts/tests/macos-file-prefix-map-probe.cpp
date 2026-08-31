#include <cstring>

const char* binaryRootFile();

namespace {

bool startsWith(const char* value, const char* prefix)
{
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

}

int main()
{
    return startsWith(__FILE__, "barrier-source/") &&
                   startsWith(binaryRootFile(), "barrier-build/")
               ? 0
               : 1;
}
