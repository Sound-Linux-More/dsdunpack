/**
* DSD Unpack - https://github.com/michaelburton/dsdunpack
*
* Copyright (c) 2014 by Michael Burton.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsdiff.h"
#include "dsdio.h"
#include "dst_decoder.h"
#include <pthread.h>

/* Large file seeking on MSVC using the same syntax as GCC */
#ifdef _MSC_VER
#define fseeko _fseeki64
#define ftello _ftelli64
#define off_t int64_t
#endif

typedef struct dsdiff_read_context_t {
    chunk_header_t  current_chunk;
    uint64_t        bytes_read;
    off_t           next_chunk;

    uint8_t        *fake_id3;
    uint64_t        fake_id3_len;

    dst_decoder_t  *dst_decoder;
    uint32_t        dst_frame_size;
    uint32_t        dst_frames_remain;
    char           *dest_buffer;
    uint32_t        dest_buffer_frames_remain;
    pthread_cond_t  dst_decode_done;
    pthread_mutex_t dst_decode_done_mutex;
} dsdiff_read_context_t;

static void dsdiff_dst_decode_done(uint8_t *frame_data, size_t frame_size, void *userdata)
{
    dsdiff_read_context_t *context = (dsdiff_read_context_t*) userdata;

    memcpy(context->dest_buffer, frame_data, frame_size);
    context->dest_buffer += frame_size;

    if (!--context->dest_buffer_frames_remain) {
        pthread_mutex_lock(&context->dst_decode_done_mutex);
        pthread_cond_signal(&context->dst_decode_done);
        pthread_mutex_unlock(&context->dst_decode_done_mutex);
    }
}

static void dsdiff_dst_decode_error(int frame_count, int frame_error_code, const char *frame_error_message, void *userdata)
{
    fprintf(stderr, "DST decoding error %d: %s\n", frame_error_code, frame_error_message);
}

static int dsdiff_read_open(FILE *fp, dsd_reader_t *reader)
{
    uint8_t *fake_id3 = NULL;
    uint64_t fake_id3_len = 0;

    /* Check FRM8 header */
    {
        form_dsd_chunk_t frm8;
        fread(&frm8, FORM_DSD_CHUNK_SIZE, 1, fp);
        if (frm8.chunk_id != FRM8_MARKER || frm8.form_type != DSD_MARKER) {
            return 0;
        }
    }

    /* Check file version chunk */
    {
        format_version_chunk_t fver;
        fread(&fver, FORMAT_VERSION_CHUNK_SIZE, 1, fp);
        if (fver.chunk_id != FVER_MARKER) {
            return 0;
        }
        if (hton64(fver.chunk_data_size) > 4) {
            fseeko(fp, CEIL_ODD_NUMBER(SWAP64(fver.chunk_data_size)) - 4, SEEK_CUR);
        }
    }

    /* Read audio properties */
    {
        property_chunk_t prop;
        char *props, *cur_prop;

        /* Search for PROP chunk with SND properties, which is guaranteed to be before the sound data */
        prop.chunk_id = 0;
        for (;;) {
            fread(&prop, PROPERTY_CHUNK_SIZE, 1, fp);
            SWAP64(prop.chunk_data_size);
            if (prop.chunk_id == PROP_MARKER && prop.property_type == SND_MARKER) {
                break;
            } else if (feof(fp) || prop.chunk_id == DSD_MARKER || prop.chunk_id == DST_MARKER) {
                return 0;
            } else {
                fseeko(fp, CEIL_ODD_NUMBER(prop.chunk_data_size) - 4, SEEK_CUR);
            }
        }

        /* Read and process property sub-chunks */
        props = malloc((size_t) CEIL_ODD_NUMBER(prop.chunk_data_size) - 4);
        fread(props, (size_t) CEIL_ODD_NUMBER(prop.chunk_data_size) - 4, 1, fp);
        cur_prop = props;
        while (cur_prop < (props + prop.chunk_data_size - 4)) {
            chunk_header_t *prop_head = (chunk_header_t*) cur_prop;
            if (prop_head->chunk_id == FS_MARKER) {
                sample_rate_chunk_t *samp = (sample_rate_chunk_t*) cur_prop;
                reader->sample_rate = hton32(samp->sample_rate);
            } else if (prop_head->chunk_id == CHNL_MARKER) {
                channels_chunk_t *chnl = (channels_chunk_t*) cur_prop;
                reader->channel_count = (uint8_t) hton16(chnl->channel_count);
            } else if (prop_head->chunk_id == MAKE_MARKER('I', 'D', '3', ' ')) {
                /* Some versions of sacd-ripper put ID3 tags in PROP instead of a chunk
                   at the end of the file, so we pretend it's at the end. */
                fake_id3_len = hton64(prop_head->chunk_data_size);
                fake_id3 = malloc((size_t) fake_id3_len);
                memcpy(fake_id3, cur_prop + CHUNK_HEADER_SIZE, (size_t) fake_id3_len);
            }
            cur_prop += CEIL_ODD_NUMBER(hton64(prop_head->chunk_data_size)) + CHUNK_HEADER_SIZE;
        }
        free(props);
    }

    /* And finally, prepare to read audio data */
    {
        chunk_header_t audio;
        dsdiff_read_context_t *context = (dsdiff_read_context_t*) malloc(sizeof(dsdiff_read_context_t));

        /* Find where audio data starts */
        for (;;) {
            fread(&audio, CHUNK_HEADER_SIZE, 1, fp);
            SWAP64(audio.chunk_data_size);
            if (audio.chunk_id == DST_MARKER) {
                dst_frame_information_chunk_t frte;
                chunk_header_t dsti;
                off_t start;
                
                fread(&frte, DST_FRAME_INFORMATION_CHUNK_SIZE, 1, fp);
                context->dst_frame_size = reader->sample_rate / hton16(frte.frame_rate) / 8 * reader->channel_count;
                context->dst_frames_remain = hton32(frte.num_frames);
                reader->data_length = (uint64_t) context->dst_frames_remain * context->dst_frame_size;
                reader->compressed = 1;

                context->dst_decoder = dst_decoder_create(reader->channel_count, reader->sample_rate / 44100, dsdiff_dst_decode_done, dsdiff_dst_decode_error, context);
                context->dst_decode_done = (pthread_cond_t) PTHREAD_COND_INITIALIZER;
                context->dst_decode_done_mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

                /* The next 'real' chunk is after the DSTI, so find where the DSTI chunk ends... */
                start = ftello(fp);
                fseeko(fp, CEIL_ODD_NUMBER(audio.chunk_data_size - DST_FRAME_INFORMATION_CHUNK_SIZE), SEEK_CUR);
                fread(&dsti, CHUNK_HEADER_SIZE, 1, fp);
                context->next_chunk = ftello(fp) + CEIL_ODD_NUMBER(hton64(dsti.chunk_data_size));
                fseeko(fp, start, SEEK_SET);
                break;
            } else if (audio.chunk_id == DSD_MARKER) {
                reader->data_length = audio.chunk_data_size;
                reader->compressed = 0;
                context->next_chunk = ftello(fp) + CEIL_ODD_NUMBER(audio.chunk_data_size);
                context->dst_decoder = NULL;
                break;
            } else if (feof(fp)) {
                if (fake_id3) {
                    free(fake_id3);
                }
                free(context);
                return 0;
            } else {
                fseeko(fp, CEIL_ODD_NUMBER(audio.chunk_data_size), SEEK_CUR);
            }
        }
        context->current_chunk = audio;
        context->bytes_read = 0;
        context->fake_id3 = fake_id3;
        context->fake_id3_len = fake_id3_len;
        reader->private = context;
    }

    return 1;
}

static size_t dsdiff_read_samples(char *buf, size_t len, dsd_reader_t *reader)
{
    dsdiff_read_context_t *context = (dsdiff_read_context_t*) reader->private;
    size_t amount = 0;

    if (context->current_chunk.chunk_id == DST_MARKER) {
        uint32_t frame_count = len / context->dst_frame_size;
        chunk_header_t dstf;
        uint32_t i;
        uint8_t *frame = malloc(context->dst_frame_size);
        
        if (frame_count > context->dst_frames_remain) {
            frame_count = context->dst_frames_remain;
        }

        context->dest_buffer = buf;
        context->dest_buffer_frames_remain = frame_count;
        context->dst_frames_remain -= frame_count;
        for (i = 0; i < frame_count; i++) {
            /* Read DST frame and add to decoder queue */
            fread(&dstf, DST_FRAME_DATA_CHUNK_SIZE, 1, reader->input);
            SWAP64(dstf.chunk_data_size);
            if (dstf.chunk_id == DSTC_MARKER) {
                /* Ignore CRC chunks */
                fseeko(reader->input, CEIL_ODD_NUMBER(dstf.chunk_data_size), SEEK_CUR);
                continue;
            } else if (dstf.chunk_id != DSTF_MARKER) {
                context->dst_frames_remain = 0;
                context->dest_buffer_frames_remain -= frame_count - i;
                break;
            }
            fread(frame, 1, (size_t) CEIL_ODD_NUMBER(dstf.chunk_data_size), reader->input);
            dst_decoder_decode(context->dst_decoder, frame, (size_t) dstf.chunk_data_size);
        }
        free(frame);

        if (frame_count > 0) {
            pthread_mutex_lock(&context->dst_decode_done_mutex);
            pthread_cond_wait(&context->dst_decode_done, &context->dst_decode_done_mutex);
            pthread_mutex_unlock(&context->dst_decode_done_mutex);
        }
        amount = context->dest_buffer - buf;
    } else if (context->next_chunk == 0 && context->fake_id3) {
        /* Reached EOF alerady, so read from the 'fake' ID3 chunk */
        uint64_t bytes_remain = context->fake_id3_len - context->bytes_read;
        amount = (len > bytes_remain) ? (size_t) bytes_remain : len;
        memcpy(buf, context->fake_id3 + context->bytes_read, amount);
        context->bytes_read += amount;
    } else {
        uint64_t bytes_remain = context->current_chunk.chunk_data_size - context->bytes_read;
        amount = (len > bytes_remain) ? (size_t) bytes_remain : len;
        amount = fread(buf, 1, amount, reader->input);
        context->bytes_read += amount;
    }

    return amount;
}

static uint32_t dsdiff_read_next_chunk(dsd_reader_t *reader)
{
    dsdiff_read_context_t *context = (dsdiff_read_context_t*) reader->private;

    if (context->next_chunk) {
        context->bytes_read = 0;
        fseeko(reader->input, context->next_chunk, SEEK_SET);
        if (fread(&context->current_chunk, CHUNK_HEADER_SIZE, 1, reader->input) == 1) {
            SWAP64(context->current_chunk.chunk_data_size);
            context->next_chunk = ftello(reader->input) + CEIL_ODD_NUMBER(context->current_chunk.chunk_data_size);
        } else {
            context->current_chunk.chunk_id = context->fake_id3 ? MAKE_MARKER('I', 'D', '3', ' ') : 0;
            context->current_chunk.chunk_data_size = 0;
            context->next_chunk = 0;
        }
    } else if (context->fake_id3) {
        context->current_chunk.chunk_id = 0;
        free(context->fake_id3);
        context->fake_id3 = 0;
    }

    return context->current_chunk.chunk_id;
}

static void dsdiff_read_close(dsd_reader_t *reader)
{
    dsdiff_read_context_t *context = (dsdiff_read_context_t*) reader->private;

    if (context->fake_id3) {
        free(context->fake_id3);
        context->fake_id3 = NULL;
    }

    if (context->dst_decoder) {
        pthread_cond_destroy(&context->dst_decode_done);
        pthread_mutex_destroy(&context->dst_decode_done_mutex);
        dst_decoder_destroy(context->dst_decoder);
        context->dst_decoder = NULL;
    }
}

dsd_reader_funcs_t *dsdiff_reader_funcs()
{
    static dsd_reader_funcs_t funcs = {
        dsdiff_read_open,
        dsdiff_read_samples,
        dsdiff_read_next_chunk,
        dsdiff_read_close
    };
    return &funcs;
}


typedef struct dsdiff_write_context_t {
    uint64_t current_chunk_bytes;
    off_t    current_chunk_start;
} dsdiff_write_context_t;

static void dsdiff_write_open(dsd_writer_t *writer)
{
    {
        form_dsd_chunk_t frm8;
        frm8.chunk_id = FRM8_MARKER;
        frm8.form_type = DSD_MARKER;
        fwrite(&frm8, FORM_DSD_CHUNK_SIZE, 1, writer->output);
    }

    {
        format_version_chunk_t fver;
        fver.chunk_id = FVER_MARKER;
        fver.chunk_data_size = CALC_CHUNK_SIZE(FORMAT_VERSION_CHUNK_SIZE - CHUNK_HEADER_SIZE);
        fver.version = hton32(DSDIFF_VERSION);
        fwrite(&fver, FORMAT_VERSION_CHUNK_SIZE, 1, writer->output);
    }

    {
        property_chunk_t prop;

        prop.chunk_id = PROP_MARKER;
        prop.chunk_data_size = CALC_CHUNK_SIZE(PROPERTY_CHUNK_SIZE - CHUNK_HEADER_SIZE
            + SAMPLE_RATE_CHUNK_SIZE + CHANNELS_CHUNK_SIZE + writer->channel_count * sizeof(uint32_t)
            + COMPRESSION_TYPE_CHUNK_SIZE + 14 /* "not compressed" */
            + LOADSPEAKER_CONFIG_CHUNK_SIZE);
        prop.property_type = SND_MARKER;
        fwrite(&prop, PROPERTY_CHUNK_SIZE, 1, writer->output);
    }

    {
        sample_rate_chunk_t fs;
        
        fs.chunk_id = FS_MARKER;
        fs.chunk_data_size = CALC_CHUNK_SIZE(SAMPLE_RATE_CHUNK_SIZE - CHUNK_HEADER_SIZE);
        fs.sample_rate = hton32(writer->sample_rate);
        fwrite(&fs, SAMPLE_RATE_CHUNK_SIZE, 1, writer->output);
    }

    {
        channels_chunk_t chnl;
        int i;

        chnl.chunk_id = CHNL_MARKER;
        chnl.chunk_data_size = CALC_CHUNK_SIZE(CHANNELS_CHUNK_SIZE - CHUNK_HEADER_SIZE + writer->channel_count * sizeof(uint32_t));
        chnl.channel_count = hton16(writer->channel_count);
        switch (writer->channel_count) {
        case 2:
            chnl.channel_ids[0] = SLFT_MARKER;
            chnl.channel_ids[1] = SRGT_MARKER;
            break;
        case 5:
            chnl.channel_ids[0] = MLFT_MARKER;
            chnl.channel_ids[1] = MRGT_MARKER;
            chnl.channel_ids[2] = C_MARKER;
            chnl.channel_ids[3] = LS_MARKER;
            chnl.channel_ids[4] = RS_MARKER;
            break;
        case 6:
            chnl.channel_ids[0] = MLFT_MARKER;
            chnl.channel_ids[1] = MRGT_MARKER;
            chnl.channel_ids[2] = C_MARKER;
            chnl.channel_ids[3] = LFE_MARKER;
            chnl.channel_ids[4] = LS_MARKER;
            chnl.channel_ids[5] = RS_MARKER;
            break;
        default:
            for (i = 0; i < writer->channel_count; i++) {
                sprintf((char*) &chnl.channel_ids[i], "C%03i", i);
            }
        }
        fwrite(&chnl, CHANNELS_CHUNK_SIZE + writer->channel_count * sizeof(uint32_t), 1, writer->output);
    }

    {
        compression_type_chunk_t cmpr;

        cmpr.chunk_id = CMPR_MARKER;
        cmpr.chunk_data_size = CALC_CHUNK_SIZE(COMPRESSION_TYPE_CHUNK_SIZE - CHUNK_HEADER_SIZE + 14);
        cmpr.compression_type = DSD_MARKER;
        cmpr.count = 14;
        strcpy(cmpr.compression_name, "not compressed");
        fwrite(&cmpr, CEIL_ODD_NUMBER(COMPRESSION_TYPE_CHUNK_SIZE + 14), 1, writer->output);
    }

    {
        loudspeaker_config_chunk_t lsco;

        lsco.chunk_id = LSCO_MARKER;
        lsco.chunk_data_size = CALC_CHUNK_SIZE(LOADSPEAKER_CONFIG_CHUNK_SIZE - CHUNK_HEADER_SIZE);
        switch (writer->channel_count) {
        case 2:
            lsco.loudspeaker_config = hton16(LS_CONFIG_2_CHNL);
            break;
        case 5:
            lsco.loudspeaker_config = hton16(LS_CONFIG_5_CHNL);
            break;
        case 6:
            lsco.loudspeaker_config = hton16(LS_CONFIG_6_CHNL);
            break;
        default:
            lsco.loudspeaker_config = hton16(LS_CONFIG_UNDEFINED);
        }
        fwrite(&lsco, LOADSPEAKER_CONFIG_CHUNK_SIZE, 1, writer->output);
    }

    {
        dsd_sound_data_chunk_t dsd;
        dsdiff_write_context_t *context = (dsdiff_write_context_t*) malloc(sizeof(dsdiff_write_context_t));

        fflush(writer->output);
        context->current_chunk_start = ftello(writer->output);
        context->current_chunk_bytes = 0;
        writer->private = context;

        dsd.chunk_id = DSD_MARKER;
        fwrite(&dsd, DSD_SOUND_DATA_CHUNK_SIZE, 1, writer->output);
    }
}

static void dsdiff_write_samples(const char *buf, size_t len, dsd_writer_t *writer)
{
    dsdiff_write_context_t *context = (dsdiff_write_context_t*) writer->private;
    size_t written = fwrite(buf, 1, len, writer->output);
    context->current_chunk_bytes += written;
    writer->data_length += written;
}

static void dsdiff_write_finish_chunk(dsdiff_write_context_t *context, dsd_writer_t *writer)
{
    if (context->current_chunk_bytes & 1) {
        uint8_t padding = 0;
        fwrite(&padding, 1, 1, writer->output);
    }

    fflush(writer->output);
    fseeko(writer->output, context->current_chunk_start + 4, SEEK_SET);
    context->current_chunk_bytes = CALC_CHUNK_SIZE(context->current_chunk_bytes);
    fwrite(&context->current_chunk_bytes, sizeof(uint64_t), 1, writer->output);
    fflush(writer->output);
    fseeko(writer->output, 0, SEEK_END);
}

static int dsdiff_write_next_chunk(uint32_t chunk, dsd_writer_t *writer)
{
    dsdiff_write_context_t *context = (dsdiff_write_context_t*) writer->private;
    chunk_header_t header;

    dsdiff_write_finish_chunk(context, writer);
    context->current_chunk_start = ftello(writer->output);
    context->current_chunk_bytes = 0;

    header.chunk_id = chunk;
    fwrite(&header, CHUNK_HEADER_SIZE, 1, writer->output);

    return 1;
}

static void dsdiff_write_close(dsd_writer_t *writer)
{
    dsdiff_write_context_t *context = (dsdiff_write_context_t*) writer->private;
    uint64_t file_end;

    dsdiff_write_finish_chunk(context, writer);
    file_end = hton64(ftello(writer->output) - CHUNK_HEADER_SIZE);
    /* Write the length of the FRM8 chunk */
    fseeko(writer->output, 4, SEEK_SET);
    fwrite(&file_end, sizeof(uint64_t), 1, writer->output);
}

dsd_writer_funcs_t *dsdiff_writer_funcs()
{
    static dsd_writer_funcs_t funcs = {
        dsdiff_write_open,
        dsdiff_write_samples,
        dsdiff_write_next_chunk,
        dsdiff_write_close
    };
    return &funcs;
}
