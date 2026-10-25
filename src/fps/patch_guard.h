// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BB_FPS_PATCH_GUARD_H
#define BB_FPS_PATCH_GUARD_H
#include "timing.h"
#include <stdio.h>
// Scan BBPATCH2 entries from the current position before the loader writes any.
// 0 valid, 1 malformed/truncated, 2 overlaps timing. Restore the file position.
static inline int bb_fps_validate_patches(FILE *f, uint64_t count) {
    long entries=ftell(f), end;
    int result=1;
    if (entries<0 || fseek(f,0,SEEK_END)) return 1;
    end=ftell(f);
    if (end<entries || fseek(f,entries,SEEK_SET)) return 1;
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset, length;
        if (fread(&offset,8,1,f)!=1 || fread(&length,8,1,f)!=1) goto done;
        long at=ftell(f);
        if (!length || length>4096 || at<0 || at>end || length>(uint64_t)(end-at)) goto done;
        if (bb_fps_conflicts(offset,length)) { result=2; goto done; }
        if (fseek(f,(long)length,SEEK_CUR)) goto done;
    }
    result=ftell(f)==end ? 0 : 1;
done:
    if (fseek(f,entries,SEEK_SET)) return 1;
    return result;
}
#endif
