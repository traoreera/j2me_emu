#pragma once

#include <cstdint>
#include <cstddef>

namespace hal
{

struct FileHandle
{
    void *opaque = nullptr;
};

bool file_open(FileHandle *out, const char *path);
void file_close(FileHandle *fh);
bool file_seek(FileHandle *fh, int32_t offset, int whence);
size_t file_read(FileHandle *fh, uint8_t *buf, size_t len);
int32_t file_tell(FileHandle *fh);
int32_t file_size(FileHandle *fh);

} // namespace hal