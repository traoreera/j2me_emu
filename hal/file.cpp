#include "hal/file.h"
#include <cstdio>

namespace hal
{

    bool file_open(FileHandle *out, const char *path)
    {
        FILE *f = std::fopen(path, "rb");
        if (!f)
            return false;
        out->opaque = f;
        return true;
    }

    void file_close(FileHandle *fh)
    {
        if (fh->opaque)
        {
            std::fclose(static_cast<FILE *>(fh->opaque));
            fh->opaque = nullptr;
        }
    }

    bool file_seek(FileHandle *fh, int32_t offset, int whence)
    {
        FILE *f = static_cast<FILE *>(fh->opaque);
        return std::fseek(f, offset, whence) == 0;
    }

    size_t file_read(FileHandle *fh, uint8_t *buf, size_t len)
    {
        FILE *f = static_cast<FILE *>(fh->opaque);
        return std::fread(buf, 1, len, f);
    }

    int32_t file_tell(FileHandle *fh)
    {
        FILE *f = static_cast<FILE *>(fh->opaque);
        return std::ftell(f);
    }

    int32_t file_size(FileHandle *fh)
    {
        FILE *f = static_cast<FILE *>(fh->opaque);
        int32_t cur = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        int32_t sz = std::ftell(f);
        std::fseek(f, cur, SEEK_SET);
        return sz;
    }

} // namespace hal