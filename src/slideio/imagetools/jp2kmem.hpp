#pragma once
#include <openjpeg.h>

typedef struct
{
    OPJ_UINT8* pData;
    OPJ_SIZE_T dataSize;
    OPJ_SIZE_T offset;
} opj_memory_stream;

opj_stream_t* opj_stream_create_default_memory_stream(opj_memory_stream* memoryStream, OPJ_BOOL is_read_stream);
