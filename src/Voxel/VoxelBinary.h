#pragma once

// VX.1: private byte I/O shared by the voxel codecs. Explicit little-endian at every width, independent of the host, and every
// size computation checked. Not for use outside src/Voxel.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace Voxel::Binary
{
inline constexpr std::size_t ChecksumBytes = 8;

inline std::uint64_t Fnv1a(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;

    for(const std::byte value : bytes)
    {
        hash = (hash ^ static_cast<std::uint64_t>(value)) * 1099511628211ull;
    }

    return hash;
}

inline std::optional<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b) noexcept
{
    if(a > UINT64_MAX - b)
    {
        return std::nullopt;
    }

    return a + b;
}

inline std::optional<std::uint64_t> CheckedMultiply(std::uint64_t a, std::uint64_t b) noexcept
{
    if(a != 0 && b > UINT64_MAX / a)
    {
        return std::nullopt;
    }

    return a * b;
}

// Writes into a buffer sized up front, so encoding a large region never reallocates. Overrunning the size is a logic error.
class Writer
{
public:
    explicit Writer(std::size_t size) :
        m_bytes(size)
    {
    }

    void U8(std::uint8_t value)
    {
        Raw(value, 1);
    }

    void I8(std::int8_t value)
    {
        Raw(static_cast<std::uint8_t>(value), 1);
    }

    void U16(std::uint16_t value)
    {
        Raw(value, 2);
    }

    void U32(std::uint32_t value)
    {
        Raw(value, 4);
    }

    void I32(std::int32_t value)
    {
        Raw(static_cast<std::uint32_t>(value), 4);
    }

    void U64(std::uint64_t value)
    {
        Raw(value, 8);
    }

    void Bytes(const void* data, std::size_t size)
    {
        Reserve(size);
        if(size != 0)
        {
            std::memcpy(m_bytes.data() + m_at, data, size);
        }
        m_at += size;
    }

    std::size_t Position() const noexcept
    {
        return m_at;
    }

    // Appends the checksum of everything written so far and returns the finished buffer.
    std::vector<std::byte> Finish()
    {
        U64(Fnv1a(std::span(m_bytes.data(), m_at)));

        if(m_at != m_bytes.size())
        {
            throw std::logic_error("Voxel encoder size mismatch.");
        }

        return std::move(m_bytes);
    }

private:
    void Reserve(std::size_t size)
    {
        if(m_bytes.size() - m_at < size)
        {
            throw std::logic_error("Voxel encoder overrun.");
        }
    }

    void Raw(std::uint64_t value, std::size_t size)
    {
        Reserve(size);

        for(std::size_t index = 0; index < size; ++index)
        {
            m_bytes[m_at + index] = static_cast<std::byte>((value >> (8 * index)) & 0xFF);
        }

        m_at += size;
    }

    std::vector<std::byte> m_bytes;
    std::size_t m_at = 0;
};

// Every read is bounds-checked. A failed read leaves `ok` false and returns zero; later reads keep failing.
class Reader
{
public:
    explicit Reader(std::span<const std::byte> bytes) :
        m_bytes(bytes)
    {
    }

    std::uint8_t U8()
    {
        return static_cast<std::uint8_t>(Raw(1));
    }

    std::int8_t I8()
    {
        return static_cast<std::int8_t>(static_cast<std::uint8_t>(Raw(1)));
    }

    std::uint16_t U16()
    {
        return static_cast<std::uint16_t>(Raw(2));
    }

    std::uint32_t U32()
    {
        return static_cast<std::uint32_t>(Raw(4));
    }

    std::int32_t I32()
    {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(Raw(4)));
    }

    std::uint64_t U64()
    {
        return Raw(8);
    }

    // A view of the next `size` bytes, or an empty span (with ok false) when they are not all there.
    std::span<const std::byte> Take(std::uint64_t size)
    {
        if(!ok || Remaining() < size)
        {
            ok = false;
            return {};
        }

        const auto view = m_bytes.subspan(m_at, static_cast<std::size_t>(size));
        m_at += static_cast<std::size_t>(size);
        return view;
    }

    std::size_t Position() const noexcept
    {
        return m_at;
    }

    std::size_t Remaining() const noexcept
    {
        return m_bytes.size() - m_at;
    }

    bool ok = true;

private:
    std::uint64_t Raw(std::size_t size)
    {
        if(!ok || Remaining() < size)
        {
            ok = false;
            return 0;
        }

        std::uint64_t value = 0;

        for(std::size_t index = 0; index < size; ++index)
        {
            value |= static_cast<std::uint64_t>(m_bytes[m_at + index]) << (8 * index);
        }

        m_at += size;
        return value;
    }

    std::span<const std::byte> m_bytes;
    std::size_t m_at = 0;
};

// Little-endian u32 / u64 at a byte position of a buffer already known to be large enough.
inline std::uint32_t LoadU32(std::span<const std::byte> bytes, std::size_t at) noexcept
{
    std::uint32_t value = 0;
    for(std::size_t index = 0; index < 4; ++index)
    {
        value |= static_cast<std::uint32_t>(bytes[at + index]) << (8 * index);
    }
    return value;
}

inline std::uint64_t LoadU64(std::span<const std::byte> bytes, std::size_t at) noexcept
{
    std::uint64_t value = 0;
    for(std::size_t index = 0; index < 8; ++index)
    {
        value |= static_cast<std::uint64_t>(bytes[at + index]) << (8 * index);
    }
    return value;
}
}
