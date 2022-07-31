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
#include "getopt.h"
#ifdef PTW32_STATIC_LIB
#include <pthread.h>
#endif

#include "dsdio.h"

#define BUFFER_SIZE 262144 /* Size of read buffer */


static struct opts_s {
    int         output_dsf;
    int         output_dsdiff;
    int         ignore_tags;
    int         verbose;
    const char *input_file;
    const char *output_file;
} opts;


/* Parse command-line options. */
static int parse_options(int argc, char *argv[])
{
    int opt; /* used for argument parsing */
    char *program_name;

    static const char help_text[] =
        "Usage: %s [options] inputfile outputfile\n"
        "  -p, --output-dsdiff             : output as Philips DSDIFF (.dff) file\n"
        "  -s, --output-dsf                : output as Sony DSF (.dsf) file\n"
        "  -t, --ignore-tags               : ignore (do not copy) ID3 tags\n"
        "  -v, --verbose                   : print file info and progress\n"
        "  inputfile                       : source file\n"
        "  outputfile                      : target file\n"
        " If no output format is specified, it is detected from output file name.\n"
        "\n"
        "Help options:\n"
        "  -?, --help                      : Show this help message\n"
        "  --usage                         : Display brief usage message\n";

    static const char usage_text[] =
        "Usage: %s [-p|--output-dsdiff] [-s|--output-dsf] [-t|--ignore-tags]\n"
        "  [-v|--verbose] [-?|--help] [--usage] inputfile outputfile\n";

    static const char options_string[] = "pstv?";
    static const struct option options_table[] = {
            { "output-dsdiff", no_argument, NULL, 'p' },
            { "output-dsf", no_argument, NULL, 's' },
            { "ignore-tags", no_argument, NULL, 't' },
            { "verbose", no_argument, NULL, 'v' },

            { "help", no_argument, NULL, '?' },
            { "usage", no_argument, NULL, 'u' },
            { NULL, 0, NULL, 0 }
    };

    program_name = strrchr(argv[0], '/');
    program_name = program_name ? program_name + 1 : argv[0];

    while ((opt = getopt_long(argc, argv, options_string, options_table, NULL)) >= 0) {
        switch (opt) {
        case 'p':
            opts.output_dsdiff = 1;
            break;
        case 's':
            opts.output_dsf = 1;
            break;
        case 't':
            opts.ignore_tags = 1;
            break;
        case 'v':
            opts.verbose = 1;
            break;

        case '?':
            fprintf(stdout, help_text, program_name);
            return 0;

        case 'u':
            fprintf(stderr, usage_text, program_name);
            return 0;
        }
    }

    if (opts.output_dsf && opts.output_dsdiff) {
        fprintf(stderr, "can't output in both DSF and DSDIFF\n");
        fprintf(stderr, usage_text, program_name);
        return 0;
    }

    if (optind < argc - 1) {
        opts.input_file = argv[optind++];
        opts.output_file = argv[optind++];

        /* Detect output format from filename if not specified */
        if (!opts.output_dsf && !opts.output_dsdiff) {
            size_t oflen = strlen(opts.output_file);
            if (oflen > 4 && !strcasecmp(opts.output_file + oflen - 4, ".dff")) {
                opts.output_dsdiff = 1;
            } else if (oflen > 4 && !strcasecmp(opts.output_file + oflen - 4, ".dsf")) {
                opts.output_dsf = 1;
            } else {
                fprintf(stderr, "no output format specified\n");
                fprintf(stderr, usage_text, program_name);
                return 0;
            }
        }
    } else {
        fprintf(stderr, "input or output file not specified\n");
        fprintf(stderr, usage_text, program_name);
        return 0;
    }

    return 1;
}


/* Initialize global variables. */
static void init(void) 
{
    /* Default option values. */
    opts.output_dsf    = 0;
    opts.output_dsdiff = 0;
    opts.ignore_tags   = 0;
    opts.verbose       = 0;
    opts.input_file    = NULL;
    opts.output_file   = NULL;
}


int main(int argc, char* argv[])
{
    int result = 1;

#ifdef PTW32_STATIC_LIB
    pthread_win32_process_attach_np();
    pthread_win32_thread_attach_np();
#endif

    init();

    if (parse_options(argc, argv)) {
        FILE *in_file;

        if ((in_file = fopen(opts.input_file, "rb")) != NULL) {
            dsd_reader_t reader;

            if (dsd_reader_open(in_file, &reader) == 1) {
                FILE *out_file;

                if (opts.verbose) {
                    uint64_t sample_count = reader.data_length * 8 / reader.channel_count;

                    if (reader.container_format == DSD_FORMAT_DSF) {
                        printf("Source file is DSF\n");
                    } else if (reader.compressed) {
                        printf("Source file is DST-compressed DSDIFF\n");
                    } else {
                        printf("Source file is uncompressed DSDIFF\n");
                    }
                    printf("Uncompressed DSD size: %" PRIu64 ", sample rate: %" PRIu32 ", channels: %" PRIu8 "\n",
                        reader.data_length, reader.sample_rate, reader.channel_count);
                    printf("Duration: %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64 " (%" PRIu64 " samples)\n",
                        sample_count / reader.sample_rate / 3600, (sample_count / reader.sample_rate / 60) % 60,
                        (sample_count / reader.sample_rate) % 60, (sample_count * 1000 / reader.sample_rate) % 1000,
                        sample_count);
                }

                if ((out_file = fopen(opts.output_file, "wb")) != NULL) {
                    dsd_writer_t writer;
                    char* buffer = malloc(BUFFER_SIZE);
                    size_t length;
                    uint32_t ext;

                    dsd_writer_open(out_file, opts.output_dsdiff ? DSD_FORMAT_DSDIFF : DSD_FORMAT_DSF,
                        reader.sample_rate, reader.channel_count, &writer);

                    /* Main audio data */
                    while ((length = dsd_reader_read(buffer, BUFFER_SIZE, &reader)) > 0) {
                        dsd_writer_write(buffer, length, &writer);
                        if (opts.verbose) {
                            printf("\r%2" PRIu64 "%%", writer.data_length * 100 / reader.data_length);
                        }
                    }

                    if (opts.verbose) {
                        printf("\n");
                    }

                    /* Format-specific extensions (DSDIFF comment/edit master, ID3 tags, etc.) */
                    while ((ext = dsd_reader_next_chunk(&reader)) > 0) {
                        if ((!opts.ignore_tags || ext != MAKE_MARKER('I', 'D', '3', ' '))
                            && dsd_writer_next_chunk(ext, &writer)) {
                            if (opts.verbose) {
                                char *extc = (char*) &ext;
                                printf("Writing %c%c%c%c...\n", extc[0], extc[1], extc[2], extc[3]);
                            }
                            while ((length = dsd_reader_read(buffer, BUFFER_SIZE, &reader)) > 0) {
                                dsd_writer_write(buffer, length, &writer);
                            }
                        }
                    }

                    dsd_writer_close(&writer);

                    result = 0; /* Success! */
                } else {
                    fprintf(stderr, "could not open output file \"%s\"\n", opts.output_file);
                }

                dsd_reader_close(&reader);
            } else {
                fprintf(stderr, "input file is not valid DSF or DSDIFF\n");
                fclose(in_file);
            }
        } else {
            fprintf(stderr, "could not open input file \"%s\"\n", opts.input_file);
        }
    }

#ifdef PTW32_STATIC_LIB
    pthread_win32_thread_detach_np();
    pthread_win32_process_detach_np();
#endif

    return result;
}
