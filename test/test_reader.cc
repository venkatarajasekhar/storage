#include "../include/storage_api.h"
#include "test_reader.h"
#include "../util/crc32c.h"
#include "../util/coding.h"
#include "../include/storage.h"

#define ERR_STREAM_INFO_CHECk_FAILED 9000
#define ERR_CRC_CHECK_FAILED 9001

void FrameReader::Start()
{
    Create();

    return;
}

int32_t FrameReader::GenerateListRangeTime(UTIME_T &start, UTIME_T &end)
{
    int min;
    int max;
    struct timeval now;

    /* 生成过去2小时的一段时间 */
    int a = rand() % 7200;
    int b = rand() % 7200;

    if (a > b)
    {
        min = b;
        max = a;
    }
    else
    {
        min = a;
        max = b;
    }

    gettimeofday(&now, NULL);

    start.seconds = now.tv_sec - max;
    start.nseconds = now.tv_usec * 1000;
    end.seconds = now.tv_sec - min;
    end.nseconds = now.tv_usec * 1000;

    return 0;
}

int32_t FrameReader::CheckFrame(FRAME_INFO_T *frame_info)
{
    int ret = 0;
    char length = frame_info->size;
    char *buffer = frame_info->buffer;

    uint32_t actual_crc = crc32c::Value(buffer, length - 4);

    ret = memcmp(buffer, stream_info_, 64);
    if (ret != 0)
    {
        return -ERR_STREAM_INFO_CHECk_FAILED;
    }
    buffer += 64;

    int seq = DecodeFixed32(buffer);
    buffer += 4;

    buffer += length - 72;

    uint32_t expect_crc = DecodeFixed32(buffer);

    if (actual_crc != expect_crc)
    {
        return -ERR_CRC_CHECK_FAILED;
    }

    fprintf(stderr, "frame type is %d, time of frame is %d.%d, id is %d, seq is %d\n", frame_info->type, frame_info->frame_time.seconds,
        frame_info->frame_time.nseconds, id_, seq);

    return 0;
}

void *FrameReader::Entry()
{
    int i = 0;
    int32_t ret;
    UTIME_T start;
    UTIME_T end;

    for (; i < 10000; i++)
    {
        FRAME_INFO_T frame_info = {0};

        if (op_id_ < 0)
        {
            uint32_t temp;
            ret = storage_open(stream_info_, 64, 0, &temp);
            assert(ret == 0);

            op_id_ = (int32_t)temp;
        }

        GenerateListRangeTime(start, end);
        FRAGMENT_INFO_T *frag_buffer = NULL;
        uint32_t count = 0;
        ret = storage_list_record_fragments(op_id_, &start, &end, &frag_buffer, &count);
        assert(ret == 0);

        /* pick one frag */
        int rand_number = rand() % count;
        FRAGMENT_INFO_T rand_frag = frag_buffer[rand_number];

        int read_rand_offset = rand() % (rand_frag.end_time.seconds - rand_frag.start_time.seconds + 60);

        UTIME_T read_start_time;
        read_start_time.seconds = rand_frag.start_time.seconds - 30 + read_rand_offset;
        read_start_time.nseconds = rand_frag.start_time.nseconds;

        ret = storage_seek(op_id_, &read_start_time);
        if (ret != 0)
        {
            fprintf(stderr, "frag start is %d.%d, end is %d.%d, seek is %d.%d, seek ret is %d\n", start.seconds, start.nseconds, 
            end.seconds, end.nseconds, read_start_time.seconds, read_start_time.nseconds, ret);
            
            goto FreeResource;
        }

        // read 200 frame
        for (int j = 0; j < 200; j++)
        {
            ret = storage_read(op_id_, &frame_info);
            if (ret != 0)
            {
                fprintf(stderr, "frag start is %d.%d, end is %d.%d, seek is %d.%d, read ret is %d\n", start.seconds, start.nseconds, 
                end.seconds, end.nseconds, read_start_time.seconds, read_start_time.nseconds, ret);
                
                goto FreeResource;
            }

            /* check frame */
            ret = CheckFrame(&frame_info);
            if (ret != 0)
            {
                fprintf(stderr, "check frame error ,frag start is %d.%d, end is %d.%d, seek is %d.%d, read ret is %d\n", start.seconds, start.nseconds, 
                end.seconds, end.nseconds, read_start_time.seconds, read_start_time.nseconds, ret);

                goto FreeResource;
            }
        }

FreeResource:
        if (i % 1234 == 1233)
        {
            storage_close(op_id_);
            op_id_ = -1;
            sleep(10);
        }
        continue;
    }
}

void FrameReader::Shutdown()
{
    fprintf(stderr, "shutdown %d\n", id_);

    Join();
    return;
}

