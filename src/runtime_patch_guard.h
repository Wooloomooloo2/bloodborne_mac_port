/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BB_RUNTIME_PATCH_GUARD_H
#define BB_RUNTIME_PATCH_GUARD_H
#include "runtime.h"
#include <stdio.h>
/* Scan before the loader's first write and restore the entries position.
   Return 0 valid, 1 malformed, 2 protected runtime bytes; reason is static. */
static inline int runtime_validate_patch_set(FILE *f, uint64_t count, const char **reason) {
    long entries=ftell(f), end;
    int result=1;
    *reason=NULL;
    if (entries<0 || fseek(f,0,SEEK_END)) return 1;
    end=ftell(f);
    if (end<entries || fseek(f,entries,SEEK_SET)) return 1;
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset, length;
        unsigned char data[4096];
        if (fread(&offset,8,1,f)!=1 || fread(&length,8,1,f)!=1) goto done;
        long at=ftell(f);
        if (!length || length>sizeof(data) || offset>UINT64_MAX-length ||
            at<0 || at>end || length>(uint64_t)(end-at) || fread(data,1,(size_t)length,f)!=length) goto done;
        *reason=runtime_patch_conflict(offset,data,(size_t)length);
        if (*reason) { result=2; goto done; }
    }
    result=ftell(f)==end ? 0 : 1;
done:
    if (fseek(f,entries,SEEK_SET)) return 1;
    return result;
}
#endif
