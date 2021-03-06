/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
* Parts based on zstd_decompress.c copyright Yann Collet
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include <string.h>
#include "fast-lzma2.h"
#include "fl2_internal.h"
#include "mem.h"
#include "util.h"
#include "lzma2_dec.h"
#include "xxhash.h"

FL2LIB_API size_t FL2LIB_CALL FL2_findDecompressedSize(const void *src, size_t srcSize)
{
    return FLzma2Dec_UnpackSize(src, srcSize);
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompress(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize)
{
    size_t dSize;
    FL2_DCtx* const dctx = FL2_createDCtx();
    if(dctx == NULL)
        return FL2_ERROR(memory_allocation);
    dSize = FL2_decompressDCtx(dctx,
        dst, dstCapacity,
        src, compressedSize);
    FL2_freeDCtx(dctx);
    return dSize;
}

FL2LIB_API FL2_DCtx* FL2LIB_CALL FL2_createDCtx(void)
{
    DEBUGLOG(3, "FL2_createDCtx");
    FL2_DCtx* const dctx = malloc(sizeof(FL2_DCtx));
    if (dctx) {
        LzmaDec_Construct(dctx);
    }
    return dctx;
}

FL2LIB_API size_t FL2LIB_CALL FL2_freeDCtx(FL2_DCtx* dctx)
{
    if (dctx != NULL) {
        DEBUGLOG(3, "FL2_freeDCtx");
        FLzmaDec_Free(dctx);
        free(dctx);
    }
    return 0;
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompressDCtx(FL2_DCtx* dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    size_t res;
    BYTE prop = *(const BYTE*)src;
    BYTE const do_hash = prop >> FL2_PROP_HASH_BIT;
    size_t dicPos;
    const BYTE *srcBuf = src;
    size_t srcPos;
    size_t const srcEnd = srcSize - 1;

    ++srcBuf;
    --srcSize;

    prop &= FL2_LZMA_PROP_MASK;

    DEBUGLOG(4, "FL2_decompressDCtx : dict prop 0x%X, do hash %u", prop, do_hash);

    CHECK_F(FLzma2Dec_Init(dctx, prop, dst, dstCapacity));

    dicPos = dctx->dicPos;

    srcPos = srcSize;
    res = FLzma2Dec_DecodeToDic(dctx, dstCapacity, srcBuf, &srcPos, LZMA_FINISH_END);
    if (FL2_isError(res))
        return res;
    if (res == LZMA_STATUS_NEEDS_MORE_INPUT)
        return FL2_ERROR(srcSize_wrong);

    dicPos = dctx->dicPos - dicPos;

    if (do_hash) {
        XXH32_canonical_t canonical;
        U32 hash;

        DEBUGLOG(4, "Checking hash");

        if (srcEnd - srcPos < XXHASH_SIZEOF)
            return FL2_ERROR(srcSize_wrong);
        memcpy(&canonical, srcBuf + srcPos, XXHASH_SIZEOF);
        hash = XXH32_hashFromCanonical(&canonical);
        if (hash != XXH32(dst, dicPos, 0))
            return FL2_ERROR(checksum_wrong);
    }
    return dicPos;
}

typedef enum
{
    FL2DEC_STAGE_INIT,
    FL2DEC_STAGE_DECOMP,
    FL2DEC_STAGE_HASH,
    FL2DEC_STAGE_FINISHED
} DecoderStage;

typedef struct
{
    InputBlock inBlock;
    BYTE *outBuf;
    size_t bufSize;
} ThreadInfo;

typedef struct
{
    InBufNode *head;
    InBufNode *first;
    size_t numThreads;
    size_t maxThreads;
    size_t srcThread;
    size_t srcPos;
    int isWriting;
    BYTE prop;
    ThreadInfo threads[1];
} Lzma2DecMt;

struct FL2_DStream_s
{
#ifndef FL2_SINGLETHREAD
    Lzma2DecMt *decmt;
#endif
    CLzma2Dec dec;
    XXH32_state_t *xxh;
    DecoderStage stage;
    BYTE do_hash;
};

FL2LIB_API FL2_DStream* FL2LIB_CALL FL2_createDStream(void)
{
    FL2_DStream* const fds = malloc(sizeof(FL2_DStream));
    DEBUGLOG(3, "FL2_createDStream");
    if (fds) {
        LzmaDec_Construct(&fds->dec);
        fds->stage = FL2DEC_STAGE_INIT;
        fds->xxh = NULL;
        fds->do_hash = 0;
    }
    return fds;
}

FL2LIB_API size_t FL2LIB_CALL FL2_freeDStream(FL2_DStream* fds)
{
    if (fds != NULL) {
        DEBUGLOG(3, "FL2_freeDStream");
        FLzmaDec_Free(&fds->dec);
        XXH32_freeState(fds->xxh);
        free(fds);
    }
    return 0;
}

/*===== Streaming decompression functions =====*/
FL2LIB_API size_t FL2LIB_CALL FL2_initDStream(FL2_DStream* fds)
{
    DEBUGLOG(4, "FL2_initDStream");
    fds->stage = FL2DEC_STAGE_INIT;
    return 0;
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompressStream(FL2_DStream* fds, FL2_outBuffer* output, FL2_inBuffer* input)
{
    if (input->pos < input->size) {
        if (fds->stage == FL2DEC_STAGE_INIT) {
            BYTE prop = ((const BYTE*)input->src)[input->pos];
            ++input->pos;
            fds->do_hash = prop >> FL2_PROP_HASH_BIT;
            prop &= FL2_LZMA_PROP_MASK;

            CHECK_F(FLzma2Dec_Init(&fds->dec, prop, NULL, 0));

            if (fds->do_hash) {
                if (fds->xxh == NULL) {
                    DEBUGLOG(3, "Creating hash state");
                    fds->xxh = XXH32_createState();
                    if (fds->xxh == NULL)
                        return FL2_ERROR(memory_allocation);
                }
                XXH32_reset(fds->xxh, 0);
            }
            fds->stage = FL2DEC_STAGE_DECOMP;
        }
        if (fds->stage == FL2DEC_STAGE_DECOMP) {
            size_t destSize = output->size - output->pos;
            size_t srcSize = input->size - input->pos;
            size_t const res = FLzma2Dec_DecodeToBuf(&fds->dec, (BYTE*)output->dst + output->pos, &destSize, (const BYTE*)input->src + input->pos, &srcSize, LZMA_FINISH_ANY);

            DEBUGLOG(5, "Decoded %u bytes", (U32)destSize);

            if(fds->do_hash)
                XXH32_update(fds->xxh, (BYTE*)output->dst + output->pos, destSize);

            output->pos += destSize;
            input->pos += srcSize;

            if (FL2_isError(res))
                return res;
            if (res == LZMA_STATUS_FINISHED_WITH_MARK) {
                DEBUGLOG(4, "Found end mark");
                fds->stage = fds->do_hash ? FL2DEC_STAGE_HASH : FL2DEC_STAGE_FINISHED;
            }
        }
        if (fds->stage == FL2DEC_STAGE_HASH) {
            XXH32_canonical_t canonical;
            U32 hash;

            DEBUGLOG(4, "Checking hash");

            if (input->size - input->pos < XXHASH_SIZEOF)
                return 1;
            memcpy(&canonical, (BYTE*)input->src + input->pos, XXHASH_SIZEOF);
            hash = XXH32_hashFromCanonical(&canonical);
            if (hash != XXH32_digest(fds->xxh))
                return FL2_ERROR(checksum_wrong);
            fds->stage = FL2DEC_STAGE_FINISHED;
        }
    }
    return fds->stage != FL2DEC_STAGE_FINISHED;
}

static Lzma2DecMt *FL2_Lzma2DecMt_Create(unsigned numThreads)
{
    Lzma2DecMt *decmt = malloc(sizeof(Lzma2DecMt) + (numThreads - 1) * sizeof(ThreadInfo));
    if (!decmt)
        return NULL;
    decmt->head = FLzma2Dec_CreateInbufNode(NULL);
    decmt->first = decmt->head;
    decmt->numThreads = 1;
    decmt->maxThreads = numThreads;
    decmt->isWriting = 0;
    memset(decmt->threads, 0, numThreads * sizeof(ThreadInfo));
    decmt->threads[0].inBlock.first = decmt->head;
}

static int FL2_ParseMt(Lzma2DecMt* decmt)
{
    int res = CHUNK_MORE_DATA;
    InputBlock *inBlock = &decmt->threads[decmt->numThreads].inBlock;
    while (inBlock->endPos < inBlock->last->length) {
        res = FLzma2Dec_ParseInput(inBlock);
        if (res != CHUNK_CONTINUE)
            break;

    }
    if (res == CHUNK_DICT_RESET || res == CHUNK_FINAL) {
        if (FL2_AllocThread(decmt) != 0)
            return CHUNK_NO_MEMORY;
    }
    return res;
}

static int FL2_AllocThread(Lzma2DecMt* decmt)
{
    decmt->threads[decmt->numThreads].outBuf = malloc(decmt->threads[decmt->numThreads].inBlock.unpackSize);
    if(!decmt->threads[decmt->numThreads].outBuf)
        return 1;
    ++decmt->numThreads;
    return 0;
}

static size_t FL2_decompressBlockMt(FL2_DStream* fds, size_t thread)
{
    CLzma2Dec dec;
    Lzma2DecMt *decmt = fds->decmt;
    ThreadInfo *ti = &decmt->threads[thread];
    CHECK_F(FLzma2Dec_Init(&dec, decmt->prop, ti->outBuf, ti->bufSize));

    InBufNode *node = ti->inBlock.first;
    size_t inPos = ti->inBlock.startPos;
    while (1) {
        size_t srcSize = node->length - inPos;
        size_t const res = FLzma2Dec_DecodeToDic(&fds->dec, ti->bufSize - dec.dicPos, node->inBuf + inPos, &srcSize, node == ti->inBlock.last ? LZMA_FINISH_END : LZMA_FINISH_ANY);

        if (FL2_isError(res))
            return res;
        if (res == LZMA_STATUS_FINISHED_WITH_MARK) {
            DEBUGLOG(4, "Found end mark");
        }
        if (node == ti->inBlock.last)
            break;
        inPos = srcSize - node->length + LZMA_REQUIRED_INPUT_MAX;
        node = node->next;
    }

}

static void FL2_writeStreamBlocks(FL2_DStream* fds, FL2_outBuffer* output)
{

}

static size_t FL2_decompressStreamMt(FL2_DStream* fds, FL2_outBuffer* output, FL2_inBuffer* input)
{
    Lzma2DecMt *decmt = fds->decmt;
    if (decmt->isWriting) {
        FL2_writeStreamBlocks(fds, output);
    }
    if (!decmt->isWriting) {
        int res = FL2_LoadInputMt(decmt, input);
        if ((res == CHUNK_DICT_RESET && decmt->numThreads == decmt->maxThreads) || res == CHUNK_FINAL) {
            CHECK_F(FL2_decompressBlocksMt(fds));
            FL2_writeStreamBlocks(fds, output);
        }
    }
}

static int FL2_LoadInputMt(Lzma2DecMt *decmt, FL2_inBuffer* input)
{
    InputBlock *inBlock = &decmt->threads[decmt->numThreads].inBlock;
    int res = FL2_error_no_error;
    while (input->pos < input->size) {
        if (inBlock->last->length == LZMA2_MT_INPUT_SIZE) {
            inBlock->last = FLzma2Dec_CreateInbufNode(inBlock->last);
            if (!inBlock->last)
                return FL2_ERROR(memory_allocation);
            inBlock->endPos -= LZMA2_MT_INPUT_SIZE - LZMA_REQUIRED_INPUT_MAX;
            res = FL2_ParseMt(decmt);
            if (res == CHUNK_NO_MEMORY || res == CHUNK_FINAL || res == CHUNK_ERROR)
                break;
            if (res == CHUNK_DICT_RESET) {
                if (decmt->numThreads == decmt->maxThreads)
                    break;
                inBlock = &decmt->threads[decmt->numThreads].inBlock;
                inBlock->first = decmt->threads[decmt->numThreads - 1].inBlock.last;
            }
        }

        {
            size_t toread = MIN(input->size - input->pos, LZMA2_MT_INPUT_SIZE - inBlock->last->length);
            memcpy(inBlock->last->inBuf + inBlock->last->length, (BYTE*)input->src + input->pos, toread);
            inBlock->last->length += toread;
            input->pos += toread;
        }
    }
    return res;
}