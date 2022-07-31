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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "dsdio.h"

/* Forward declarations of format-specific read functions */
extern dsd_reader_funcs_t *dsdiff_reader_funcs();
extern dsd_reader_funcs_t *dsf_reader_funcs();

extern dsd_writer_funcs_t *dsdiff_writer_funcs();
extern dsd_writer_funcs_t *dsf_writer_funcs();


/* Reading */

int dsd_reader_open(FILE *fp, dsd_reader_t *reader)
{
    int result = 0;

    /* Find out the format of the input file by reading its 'magic number' */
    if (fread(&reader->container_format, 4, 1, fp) == 1) {
        rewind(fp);

        switch (reader->container_format) {
        case DSD_FORMAT_DSDIFF:
            reader->impl = dsdiff_reader_funcs();
            break;
        case DSD_FORMAT_DSF:
            reader->impl = dsf_reader_funcs();
            break;
        default:         /* Unknown format */
            reader->impl = NULL;
        }

        if (reader->impl && (result = reader->impl->open(fp, reader)) == 1) {
            reader->input = fp;
        }
    }

    return result;
}

size_t dsd_reader_read(char *buf, size_t len, dsd_reader_t *reader)
{
    return reader->impl->read(buf, len, reader);
}

uint32_t dsd_reader_next_chunk(dsd_reader_t *reader)
{
    return reader->impl->next_chunk(reader);
}

void dsd_reader_close(dsd_reader_t *reader)
{
    reader->impl->close(reader);
    if (reader->private) {
        free(reader->private);
        reader->private = NULL;
    }
    if (reader->input) {
        fclose(reader->input);
        reader->input = NULL;
    }
}

int dsd_writer_open(FILE *fp, uint32_t format, uint32_t sample_rate, uint8_t channel_count, dsd_writer_t *writer)
{
    switch (format) {
    case DSD_FORMAT_DSDIFF:
        writer->impl = dsdiff_writer_funcs();
        break;
    case DSD_FORMAT_DSF:
        writer->impl = dsf_writer_funcs();
        break;
    default:
        return 0;
    }

    writer->channel_count = channel_count;
    writer->sample_rate = sample_rate;
    writer->output = fp;
    writer->data_length = 0;

    writer->impl->open(writer);

    return 1;
}

void dsd_writer_write(const char *buf, size_t len, dsd_writer_t *writer)
{
    writer->impl->write(buf, len, writer);
}

int dsd_writer_next_chunk(uint32_t chunk, dsd_writer_t *writer)
{
    return writer->impl->next_chunk(chunk, writer);
}

void dsd_writer_close(dsd_writer_t *writer)
{
    writer->impl->close(writer);
    if (writer->private) {
        free(writer->private);
        writer->private = NULL;
    }
    if (writer->output) {
        fclose(writer->output);
        writer->output = NULL;
    }
}
