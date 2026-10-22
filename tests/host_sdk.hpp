// The slice of the Nitro SDK the player core uses, backed by memory, so the
// core's parser, decoder and loop logic run unchanged on the host.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using BOOL = int;

#define TRUE 1
#define FALSE 0

enum FSSeekFileMode
{
	FS_SEEK_SET,
	FS_SEEK_CUR,
	FS_SEEK_END
};

struct FSFile
{
	const std::vector<u8>* data;
	u32 pos;
	u32 reads; // Counted to check the read pattern.
};

inline u32 FS_GetLength(const FSFile* file)
{
	return u32(file->data->size());
}

inline BOOL FS_SeekFile(FSFile* file, s32 offset, FSSeekFileMode origin)
{
	s64 base = origin == FS_SEEK_SET ? 0 : origin == FS_SEEK_CUR ? file->pos : s64(file->data->size());
	s64 pos = base + offset;
	if (pos < 0 || pos > s64(file->data->size()))
		return FALSE;
	file->pos = u32(pos);
	return TRUE;
}

inline s32 FS_ReadFile(FSFile* file, void* dst, s32 len)
{
	u32 n = std::min<u32>(u32(len), u32(file->data->size()) - file->pos);
	std::memcpy(dst, file->data->data() + file->pos, n);
	file->pos += n;
	file->reads++;
	return s32(n);
}

inline void MI_CpuFill8(void* dst, u8 value, u32 size)
{
	std::memset(dst, value, size);
}

inline void MI_CpuCopy8(const void* src, void* dst, u32 size)
{
	std::memcpy(dst, src, size);
}
