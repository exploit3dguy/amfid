// https://github.com/bazad/blanket/blob/master/amfidupe/cdhash.c  - but without debug messages


/*
 * Cdhash computation
 * ------------------
 *
 *  The amfid patch needs to be able to compute the cdhash of a binary.
 *  This code is heavily based on the implementation in Ian Beer's triple_fetch project [1] and on
 *  the source of XNU [2].
 *
 *  [1]: https://bugs.chromium.org/p/project-zero/issues/detail?id=1247
 *  [2]: https://opensource.apple.com/source/xnu/xnu-4570.41.2/bsd/kern/ubc_subr.c.auto.html
 *
 */




#include <CommonCrypto/CommonCrypto.h>
#include <mach-o/loader.h>
#include "cdhash.h"

// Check whether the file looks like a Mach-O file.
static bool
macho_identify(const struct mach_header_64 *mh, size_t size) {
    // Check the file size and magic.
    if (size < 0x1000 || mh->magic != MH_MAGIC_64) {
        return false;
    }
    return true;
}

// Perform some basic validation on the Mach-O header. This is NOT enough to be sure that the
// Mach-O is safe!
static bool
macho_validate(const struct mach_header_64 *mh, size_t size) {
    if (!macho_identify(mh, size)) {
        return false;
    }
    // Check that the load commands fit in the file.
    if (mh->sizeofcmds > size) {
        return false;
    }
    // Check that each load command fits in the header.
    const uint8_t *lc_p = (const uint8_t *)(mh + 1);
    const uint8_t *lc_end = lc_p + mh->sizeofcmds;
    while (lc_p < lc_end) {
        const struct load_command *lc = (const struct load_command *)lc_p;
        if (lc->cmdsize >= 0x80000000) {
            return false;
        }
        const uint8_t *lc_next = lc_p + lc->cmdsize;
        if (lc_next > lc_end) {
            return false;
        }
        lc_p = lc_next;
    }
    return true;
}

// Get the next load command in a Mach-O file.
static const void *
macho_next_load_command(const struct mach_header_64 *mh, size_t size, const void *lc) {
    const struct load_command *next = lc;
    if (next == NULL) {
        next = (const struct load_command *)(mh + 1);
    } else {
        next = (const struct load_command *)((uint8_t *)next + next->cmdsize);
    }
    if ((uintptr_t)next >= (uintptr_t)(mh + 1) + mh->sizeofcmds) {
        next = NULL;
    }
    return next;
}

// Find the next load command in a Mach-O file matching the given type.
static const void *
macho_find_load_command(const struct mach_header_64 *mh, size_t size,
        uint32_t command, const void *lc) {
    const struct load_command *loadcmd = lc;
    for (;;) {
        loadcmd = macho_next_load_command(mh, size, loadcmd);
        if (loadcmd == NULL || loadcmd->cmd == command) {
            return loadcmd;
        }
    }
}

// Validate a CS_CodeDirectory and return its true length.
static size_t
cs_codedirectory_validate(CS_CodeDirectory *cd, size_t size) {
    // Make sure we at least have a CS_CodeDirectory. There's an end_earliest parameter, but
    // XNU doesn't seem to use it in cs_validate_codedirectory().
    if (size < sizeof(*cd)) {
        
        return 0;
    }
    // Validate the magic.
    uint32_t magic = ntohl(cd->magic);
    if (magic != CSMAGIC_CODEDIRECTORY) {
        
        return 0;
    }
    // Validate the length.
    uint32_t length = ntohl(cd->length);
    if (length > size) {
        
        return 0;
    }
    return length;
}

// Validate a CS_SuperBlob and return its true length.
static size_t
cs_superblob_validate(CS_SuperBlob *sb, size_t size) {
    // Make sure we at least have a CS_SuperBlob.
    if (size < sizeof(*sb)) {
       
        return 0;
    }
    // Validate the magic.
    uint32_t magic = ntohl(sb->magic);
    if (magic != CSMAGIC_EMBEDDED_SIGNATURE) {
       
        return 0;
    }
    // Validate the length.
    uint32_t length = ntohl(sb->length);
    if (length > size) {
        
        return 0;
    }
    uint32_t count = ntohl(sb->count);
    // Validate the count.
    CS_BlobIndex *index = &sb->index[count];
    if (count >= 0x10000 || (uintptr_t)index > (uintptr_t)sb + size) {
        
        return 0;
    }
    return length;
}

// Compute the cdhash of a code directory using SHA1.
static void
cdhash_sha1(CS_CodeDirectory *cd, size_t length, void *cdhash) {
    uint8_t digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(cd, (CC_LONG) length, digest);
    memcpy(cdhash, digest, CS_CDHASH_LEN);
}

// Compute the cdhash of a code directory using SHA256.
static void
cdhash_sha256(CS_CodeDirectory *cd, size_t length, void *cdhash) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(cd, (CC_LONG) length, digest);
    memcpy(cdhash, digest, CS_CDHASH_LEN);
}

// Compute the cdhash from a CS_CodeDirectory.
static bool
cs_codedirectory_cdhash(CS_CodeDirectory *cd, size_t size, void *cdhash) {
    size_t length = ntohl(cd->length);
    switch (cd->hashType) {
        case CS_HASHTYPE_SHA1:
            
            cdhash_sha1(cd, length, cdhash);
            return true;
        case CS_HASHTYPE_SHA256:
           
            cdhash_sha256(cd, length, cdhash);
            return true;
    }
    
    return false;
}

// Get the rank of a code directory.
static unsigned
cs_codedirectory_rank(CS_CodeDirectory *cd) {
    // The supported hash types, ranked from least to most preferred. From XNU's
    // bsd/kern/ubc_subr.c.
    static uint32_t ranked_hash_types[] = {
        CS_HASHTYPE_SHA1,
        CS_HASHTYPE_SHA256_TRUNCATED,
        CS_HASHTYPE_SHA256,
        CS_HASHTYPE_SHA384,
    };
    // Define the rank of the code directory as its index in the array plus one.
    for (unsigned i = 0; i < sizeof(ranked_hash_types) / sizeof(ranked_hash_types[0]); i++) {
        if (ranked_hash_types[i] == cd->hashType) {
            return (i + 1);
        }
    }
    return 0;
}

// Compute the cdhash from a CS_SuperBlob.
static bool
cs_superblob_cdhash(CS_SuperBlob *sb, size_t size, void *cdhash) {
    // Iterate through each index searching for the best code directory.
    CS_CodeDirectory *best_cd = NULL;
    unsigned best_cd_rank = 0;
    size_t best_cd_size = 0;
    uint32_t count = ntohl(sb->count);
    for (size_t i = 0; i < count; i++) {
        CS_BlobIndex *index = &sb->index[i];
        uint32_t type = ntohl(index->type);
        uint32_t offset = ntohl(index->offset);
        // Validate the offset.
        if (offset > size) {
            
            return false;
        }
        // Look for a code directory.
        if (type == CSSLOT_CODEDIRECTORY ||
                (CSSLOT_ALTERNATE_CODEDIRECTORIES <= type && type < CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)) {
            CS_CodeDirectory *cd = (CS_CodeDirectory *)((uint8_t *)sb + offset);
            size_t cd_size = cs_codedirectory_validate(cd, size - offset);
            if (cd_size == 0) {
                return false;
            }
            
            // Rank the code directory to see if it's better than our previous best.
            unsigned cd_rank = cs_codedirectory_rank(cd);
            if (cd_rank > best_cd_rank) {
                best_cd = cd;
                best_cd_rank = cd_rank;
                best_cd_size = cd_size;
            }
        }
    }
    // If we didn't find a code directory, error.
    if (best_cd == NULL) {
        
        return false;
    }
    // Hash the code directory.
    return cs_codedirectory_cdhash(best_cd, best_cd_size, cdhash);
}

// Compute the cdhash from a csblob.
static bool
csblob_cdhash(CS_GenericBlob *blob, size_t size, void *cdhash) {
    // Make sure we at least have a CS_GenericBlob.
    if (size < sizeof(*blob)) {
       
        return false;
    }
    uint32_t magic = ntohl(blob->magic);
    uint32_t length = ntohl(blob->length);
    
    // Make sure the length is sensible.
    if (length > size) {
        
        return false;
    }
    // Handle the blob.
    bool ok;
    switch (magic) {
        case CSMAGIC_EMBEDDED_SIGNATURE:
            ok = cs_superblob_validate((CS_SuperBlob *)blob, length);
            if (!ok) {
                return false;
            }
            return cs_superblob_cdhash((CS_SuperBlob *)blob, length, cdhash);
        case CSMAGIC_CODEDIRECTORY:
            ok = cs_codedirectory_validate((CS_CodeDirectory *)blob, length);
            if (!ok) {
                return false;
            }
            return cs_codedirectory_cdhash((CS_CodeDirectory *)blob, length, cdhash);
    }
    
    return false;
}

// Compute the cdhash for a Mach-O file.
static bool
compute_cdhash_macho(const struct mach_header_64 *mh, size_t size, void *cdhash) {
    // Find the code signature command.
    const struct linkedit_data_command *cs_cmd =
        macho_find_load_command(mh, size, LC_CODE_SIGNATURE, NULL);
    if (cs_cmd == NULL) {
        
        return false;
    }
    // Check that the code signature is in-bounds.
    const uint8_t *cs_data = (const uint8_t *)mh + cs_cmd->dataoff;
    const uint8_t *cs_end = cs_data + cs_cmd->datasize;
    if (!((uint8_t *)mh < cs_data && cs_data < cs_end && cs_end <= (uint8_t *)mh + size)) {
        
        return false;
    }
    // Check that the code signature data looks correct.
    return csblob_cdhash((CS_GenericBlob *)cs_data, cs_end - cs_data, cdhash);
}

bool
compute_cdhash(const void *file, size_t size, void *cdhash) {
    // Try to compute the cdhash for a Mach-O file.
    const struct mach_header_64 *mh = file;
    if (macho_identify(mh, size)) {
        if (!macho_validate(mh, size)) {
            
            return false;
        }
        return compute_cdhash_macho(mh, size, cdhash);
    }
    // What is it?
    
    return false;
}
