#include <Guid.hpp>

#include <sys/random.h>

#include <array>
#include <cerrno>
#include <system_error>

namespace xg
{
    Guid newGuid()
    {
        std::array<unsigned char, 16> bytes{};
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Android UUID entropy");
            offset += static_cast<size_t>(count);
        }
        // RFC 9562 UUIDv4: random payload, version 4 and RFC variant bits.
        bytes[6] = (bytes[6] & 0x0f) | 0x40;
        bytes[8] = (bytes[8] & 0x3f) | 0x80;
        return Guid(bytes);
    }
}
