// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/assert.h"
#include "common/types.h"

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Serialization {

/// gow3 21 Oct 2026: a truncated or garbled cache entry (a game that ended mid-write, a disk
/// error) used to trip ASSERT_MSG, which ends the port with exit code 23, or resize a vector to a
/// garbage count. The pipeline cache loaders catch this per entry and recompile instead.
struct CorruptData : std::runtime_error {
    CorruptData()
        : std::runtime_error("Invalid or corrupted deserialization container/shader cache") {}
};

template <typename T>
concept Container = requires(T t) {
    typename T::iterator;
    { t.begin() } -> std::same_as<typename T::iterator>;
    { t.end() } -> std::same_as<typename T::iterator>;
    { t.size() } -> std::convertible_to<std::size_t>;
};

struct Archive {
    void Alloc(size_t size) {
        container.resize(size);
    }

    void Grow(size_t size) {
        container.resize(container.size() + size);
    }

    void Merge(const Archive& ar) {
        container.insert(container.end(), ar.container.cbegin(), ar.container.cend());
        offset = container.size();
    }

    [[nodiscard]] size_t SizeBytes() const {
        return container.size();
    }

    u8* CurrPtr() {
        return container.data() + offset;
    }

    void Advance(size_t size) {
        if (size > container.size() - offset) {
            throw CorruptData{};
        }
        offset += size;
    }

    std::vector<u8>&& TakeOff() {
        offset = 0;
        return std::move(container);
    }

    [[nodiscard]] bool IsEoS() const {
        return offset >= container.size();
    }

    Archive() = default;
    explicit Archive(std::vector<u8>&& v) : container{v} {}

private:
    u32 offset{};
    std::vector<u8> container{};

    friend struct Writer;
    friend struct Reader;
};

struct Writer {
    template <typename T>
    void Write(const T* ptr, size_t size) {
        if (ar.offset + size >= ar.container.size()) {
            ar.Grow(size);
        }
        std::memcpy(ar.CurrPtr(), reinterpret_cast<const void*>(ptr), size);
        ar.Advance(size);
    }

    template <typename T>
        requires(!Container<T>)
    void Write(const T& value) {
        const auto size = sizeof(value);
        Write(&value, size);
    }

    void Write(const auto& v) {
        Write(v.size());
        for (const auto& elem : v) {
            Write(elem);
        }
    }

    void Write(const std::string& s) {
        Write(s.size());
        Write(s.c_str(), s.size());
    }

    Writer() = delete;
    explicit Writer(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

struct Reader {
    template <typename T>
    void Read(T* ptr, size_t size) {
        const u8* src = ar.CurrPtr();
        ar.Advance(size); // throws before the copy when fewer than `size` bytes are left
        std::memcpy(reinterpret_cast<void*>(ptr), src, size);
    }

    template <typename T>
        requires(!Container<T>)
    void Read(T& value) {
        const auto size = sizeof(value);
        Read(&value, size);
    }

    void Read(auto& v) {
        size_t num_elements{};
        Read(num_elements);
        // Every element takes at least one byte, so a larger count is garbage.
        if (num_elements > ar.container.size() - ar.offset) {
            throw CorruptData{};
        }
        for (size_t i = 0; i < num_elements; ++i) {
            v.emplace_back();
            Read(v.back());
        }
    }

    void Read(std::string& s) {
        size_t length{};
        Read(length);
        if (length > ar.container.size() - ar.offset) {
            throw CorruptData{};
        }
        s.resize(length);
        Read(s.data(), length);
    }

    Reader() = delete;
    explicit Reader(Archive& ar_) : ar{ar_} {}

    Archive& ar;
};

} // namespace Serialization
