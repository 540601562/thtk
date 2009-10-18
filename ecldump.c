/*
 * Redistribution and use in source and binary forms, with
 * or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain this list
 *    of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce this
 *    list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */
#include <config.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include "program.h"
#include "ecl.h"
#include "instr.h"
#include "util.h"

static FILE* out;

static void
print_usage()
{
    printf("Usage: %s -v {10,11,12} [OPTION]... FILE\n"
           "OPTION can be:\n"
           "  -o FILE  write output to the specified file\n"
           "  -r       raw dump\n"
           "  -p       generate parameter table\n"
           "  -h       display this help and exit\n"
           "  -V       display version information and exit\n\n"
           "Additional documentation might be available at <" PACKAGE_URL ">.\n"
           "Report bugs to <" PACKAGE_BUGREPORT ">.\n", argv0);
}

/* It would probably be easier to read the entire file at once and use pointers
 * to access the data. */
static int
open_ecl(ecl_t* ecl, FILE* f)
{
    unsigned int i;
    char magic[5] = { 0 };

    if (fread(magic, 4, 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    if (strncmp(magic, "SCPT", 4) != 0) {
        fprintf(stderr, "%s:%s: SCPT signature missing\n", argv0, current_input);
        return -1;
    }

    if (fread(&ecl->scpt, sizeof(header_scpt_t), 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    assert(ecl->scpt.unknown1 == 1);
    assert(ecl->scpt.include_offset == 0x24);
    assert(ecl->scpt.zero1 == 0);
    assert(ecl->scpt.zero2[0] == 0);
    assert(ecl->scpt.zero2[1] == 0);
    assert(ecl->scpt.zero2[2] == 0);
    assert(ecl->scpt.zero2[3] == 0);

    if (fseek(f, ecl->scpt.include_offset, SEEK_SET) == -1) {
        fprintf(stderr, "%s:%s: couldn't seek: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }

    if (fread(magic, 4, 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    if (strncmp(magic, "ANIM", 4) != 0) {
        fprintf(stderr, "%s:%s: ANIM signature missing\n", argv0, current_input);
        return -1;
    }

    if (fread(&ecl->anim_cnt, sizeof(uint32_t), 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    ecl->anim = malloc(sizeof(char*) * ecl->anim_cnt);
    for (i = 0; i < ecl->anim_cnt; ++i) {
        char buffer[256];
        util_read_asciiz(buffer, 256, f);
        ecl->anim[i] = strdup(buffer);
    }

    while (ftell(f) % 4 != 0) {
        if (fgetc(f) == EOF)
            break;
    }

    if (fread(magic, 4, 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    if (strncmp(magic, "ECLI", 4) != 0) {
        fprintf(stderr, "%s:%s: ECLI signature missing\n", argv0, current_input);
        return -1;
    }

    if (fread(&ecl->ecli_cnt, sizeof(uint32_t), 1, f) != 1) {
        fprintf(stderr, "%s:%s: couldn't read: %s\n",
            argv0, current_input, strerror(errno));
        return -1;
    }
    ecl->ecli = malloc(sizeof(char*) * ecl->ecli_cnt);
    for (i = 0; i < ecl->ecli_cnt; ++i) {
        char buffer[256];
        util_read_asciiz(buffer, 256, f);
        ecl->ecli[i] = strdup(buffer);
    }

    ecl->sub_cnt = ecl->scpt.sub_cnt;
    ecl->subs = calloc(ecl->sub_cnt, sizeof(sub_t));

    while (ftell(f) % 4 != 0) {
        if (fgetc(f) == EOF)
            break;
    }

    for (i = 0; i < ecl->sub_cnt; ++i) {
        if (fread(&ecl->subs[i].offset, sizeof(uint32_t), 1, f) != 1) {
            fprintf(stderr, "%s:%s: couldn't read: %s\n",
                argv0, current_input, strerror(errno));
            return -1;
        }
    }

    for (i = 0; i < ecl->sub_cnt; ++i) {
        /* XXX: Maximum length? */
        char buffer[256];
        util_read_asciiz(buffer, 256, f);
        ecl->subs[i].name = strdup(buffer);
    }

    for (i = 0; i < ecl->sub_cnt; ++i) {
        header_eclh_t eclh;
        sub_t* sub;

        if (fseek(f, ecl->subs[i].offset, SEEK_SET) == -1) {
            fprintf(stderr, "%s:%s: couldn't seek: %s\n",
                argv0, current_input, strerror(errno));
            return -1;
        }

        if (fread(magic, 4, 1, f) != 1) {
            fprintf(stderr, "%s:%s: couldn't read: %s\n",
                argv0, current_input, strerror(errno));
            return -1;
        }
        if (strncmp(magic, "ECLH", 4) != 0) {
            fprintf(stderr, "%s:%s: ECLH signature missing\n",
                argv0, current_input);
            return -1;
        }

        if (fread(&eclh, sizeof(header_eclh_t), 1, f) != 1) {
            fprintf(stderr, "%s:%s: couldn't read: %s\n",
                argv0, current_input, strerror(errno));
            return -1;
        }

        assert(eclh.unknown1 == 0x10);
        assert(eclh.zero[0] == 0);
        assert(eclh.zero[1] == 0);

        sub = &ecl->subs[i];
        sub->instr_cnt = 0;
        sub->raw_instrs = NULL;

        /* Unfortunately the instruction count is not provided. */
        while (1) {
            raw_instr_t rins;
            memset(&rins, 0, sizeof(raw_instr_t));
            rins.offset = ftell(f);

            /* EOF is expected for the last instruction. */
            if (fread(&rins, 16, 1, f) != 1)
                break;

            if (i + 1 != ecl->sub_cnt && ftell(f) > ecl->subs[i + 1].offset)
                break;

            assert(rins.zero == 0);
            assert(rins.size >= 16);

            rins.data_size = rins.size - 16;
            if (rins.data_size) {
                rins.data = malloc(rins.data_size);
                if (fread(rins.data, rins.data_size, 1, f) != 1) {
                    fprintf(stderr, "%s:%s: couldn't read: %s\n",
                        argv0, current_input, strerror(errno));
                    return -1;
                }
            }

            sub->instr_cnt++;
            sub->raw_instrs = realloc(sub->raw_instrs,
                                      sub->instr_cnt * sizeof(raw_instr_t));
            sub->raw_instrs[sub->instr_cnt - 1] = rins;
        }
    }

    return 0;
}

static void
ecldump_list_params(ecl_t* ecl)
{
    unsigned int i;

    char instr_params[1024][16];
    memset(instr_params, -1, 1024 * 16);

    for (i = 0; i < ecl->sub_cnt; ++i) {
        unsigned int j;
        for (j = 0; j < ecl->subs[i].instr_cnt; ++j) {
            unsigned int k;
            raw_instr_t* rins = &ecl->subs[i].raw_instrs[j];

            assert(rins->id <= 1024);
            assert(rins->param_cnt <= 15);

            switch (instr_params[rins->id][0]) {
            case -2: /* Broken. */
                break;
            case -1: /* New. */
                memset(instr_params[rins->id], 0, 16);
                if (rins->param_cnt != rins->data_size >> 2) {
                    instr_params[rins->id][0] = -2;
                    break;
                } else {
                    instr_params[rins->id][0] = rins->param_cnt;
                }
            default:
                for (k = 0; k < rins->param_cnt; ++k) {
                    if (instr_params[rins->id][k + 1] != 'i') {
                        /* TODO: This float checking stuff is pretty ugly,
                         * move it to util.c so it can be fixed more easily. */
                        int32_t integer = 0;
                        float floating = 0;
                        char buffer[128];
                        memcpy(&integer,
                               rins->data + k * sizeof(int32_t),
                               sizeof(int32_t));
                        memcpy(&floating,
                               rins->data + k * sizeof(int32_t),
                               sizeof(int32_t));
                        sprintf(buffer, "%.9f", floating);
                        if (((integer & 0xfffff000) == 0xfffff000) ||
                            (strcmp("nan", buffer) == 0) ||
                            (integer != 0 && strcmp("0.000000000", buffer) == 0))
                            instr_params[rins->id][k + 1] = 'i';
                        else 
                            instr_params[rins->id][k + 1] = 'f';
                    }
                }
                break;
            }
        }
    }

    for (i = 0; i < 1024; ++i) {
        if (instr_params[i][0] >= 0) {
            fprintf(out, "    { %u, \"%s\" },\n", i, instr_params[i] + 1);
        } else if (instr_params[i][0] == -2) {
            fprintf(out, "    /*{ %u, \"%s\" },*/\n", i, instr_params[i] + 1);
        }
    }
}

static void
ecldump_rawdump(ecl_t* ecl)
{
    unsigned int i;

    for (i = 0; i < ecl->sub_cnt; ++i) {
        unsigned int j;

        fprintf(out, "Subroutine %s\n", ecl->subs[i].name);

        for (j = 0; j < ecl->subs[i].instr_cnt; ++j) {
            raw_instr_t* rins = &ecl->subs[i].raw_instrs[j];
            unsigned int k;

            fprintf(out, "Instruction %u %u %u 0x%02x %u %u: ",
                rins->time, rins->id, rins->size, rins->param_mask,
                rins->rank_mask, rins->param_cnt);

            for (k = 0; k < rins->data_size; k += sizeof(uint32_t)) {
                fprintf(out, "0x%08x ", *(uint32_t*)(rins->data + k));
            }

            fprintf(out, "\n");
        }

        fprintf(out, "\n");
    }
}

static void
ecldump_translate(ecl_t* ecl, unsigned int version)
{
    unsigned int i;

    for (i = 0; i < ecl->sub_cnt; ++i) {
        unsigned int j;

        ecl->subs[i].instrs = malloc(sizeof(instr_t) * ecl->subs[i].instr_cnt);

        for (j = 0; j < ecl->subs[i].instr_cnt; ++j) {
            if (!instr_parse(&ecl->subs[i].raw_instrs[j],
                             &ecl->subs[i].instrs[j], version)) {
                exit(1);
            }
            ecl->subs[i].instrs[j].offset =
                ecl->subs[i].raw_instrs[j].offset - ecl->subs[i].offset;
        }
    }
}

static void
ecldump_translate_print(ecl_t* ecl)
{
    unsigned int i;

    if (ecl->anim_cnt) {
        fprintf(out, "anim { ");
        for (i = 0; i < ecl->anim_cnt; ++i)
            fprintf(out, "\"%s\"; ", ecl->anim[i]);
        fprintf(out, "}\n");
    }

    if (ecl->ecli_cnt) {
        fprintf(out, "ecli { ");
        for (i = 0; i < ecl->ecli_cnt; ++i)
            fprintf(out, "\"%s\"; ", ecl->ecli[i]);
        fprintf(out, "}\n");
    }

    for (i = 0; i < ecl->sub_cnt; ++i) {
        unsigned int j, k, m;
        unsigned int time = 0;

        fprintf(out, "\nsub %s\n{\n", ecl->subs[i].name);

        for (j = 0; j < ecl->subs[i].instr_cnt; ++j) {
            instr_t* instr = &ecl->subs[i].instrs[j];

            if (instr->time != time) {
                time = instr->time;
                fprintf(out, "%u:\n", time);
            }

            /* If there is a reference to this instruction, print a label. */
            for (k = 0; k < ecl->subs[i].instr_cnt; ++k) {
                for (m = 0; m < ecl->subs[i].instrs[k].param_cnt; ++m) {
                    if (ecl->subs[i].instrs[k].params[m].type == 'o' &&
                        instr->offset == ecl->subs[i].instrs[k].offset + ecl->subs[i].instrs[k].params[m].value.i) {
                        fprintf(out, "%s_%u:\n",
                            ecl->subs[i].name, instr->offset);
                        k = ecl->subs[i].instr_cnt;
                        break;
                    }
                }
            }

            fprintf(out, "    ins_%u", instr->id);

            if (instr->rank_mask != 0xff) {
                fprintf(out, " +");
                if (instr->rank_mask & RANK_EASY)    fputc('E', out);
                if (instr->rank_mask & RANK_NORMAL)  fputc('N', out);
                if (instr->rank_mask & RANK_HARD)    fputc('H', out);
                if (instr->rank_mask & RANK_LUNATIC) fputc('L', out);
            }

            for (k = 0; k < instr->param_cnt; ++k) {
                const char* floatb;
retry:
                switch (instr->params[k].type) {
                case 'i':
                    if (instr->param_mask & (1 << k)) {
                        if (instr->params[k].value.i >= 0 &&
                            instr->params[k].value.i % 4) {
                            fprintf(stderr, "%s:%s: strange stack offset: %d\n",
                                argv0, current_input, instr->params[k].value.i);
                            abort();
                        }
                        fprintf(out, " [%d]", instr->params[k].value.i);
                    } else
                        fprintf(out, " %d", instr->params[k].value.i);
                    break;
                case 'o':
                    fprintf(out, " %s_%u",
                        ecl->subs[i].name, instr->offset + instr->params[k].value.i);
                    break;
                case 'f':
                    floatb = util_printfloat(&instr->params[k].value.f);
                    if (instr->param_mask & (1 << k)) {
                        int tempint = instr->params[k].value.f;
                        /* XXX: Exactly how well does this work? */
                        if (floor(instr->params[k].value.f) != instr->params[k].value.f)
                            fprintf(stderr, "%s:%s: non-integral float: %s\n",
                                argv0, current_input, floatb);
                        if (tempint >= 0 && tempint % 4) {
                            fprintf(stderr, "%s:%s: strange stack offset: %s\n",
                                argv0, current_input, floatb);
                        }
                        fprintf(out, " [%sf]", floatb);
                    } else
                        fprintf(out, " %sf", floatb);
                    break;
                case 's':
                    fprintf(out, " \"%s\"", instr->params[k].value.s.data);
                    break;
                case 'c':
                    fprintf(out, " C\"%s\"", instr->params[k].value.s.data);
                    break;
                case 'D':
                    if (instr->params[k].value.D[0] == 0x6666) {
                        fprintf(out, " (float)");
                        instr->params[k].type = 'f';
                        memcpy(&instr->params[k].value.f,
                               &instr->params[k].value.D[1],
                               sizeof(float));
                    } else if (instr->params[k].value.D[0] == 0x6669) {
                        fprintf(out, " (float)");
                        instr->params[k].type = 'i';
                        memcpy(&instr->params[k].value.i,
                               &instr->params[k].value.D[1],
                               sizeof(int32_t));
                    } else if (instr->params[k].value.D[0] == 0x6969) {
                        fprintf(out, " (int)");
                        instr->params[k].type = 'i';
                        memcpy(&instr->params[k].value.i,
                               &instr->params[k].value.D[1],
                               sizeof(int32_t));
                    } else {
                        fprintf(stderr, "%s:%s: unknown value: %u\n",
                            argv0, current_input, instr->params[k].value.D[0]);
                        abort();
                    }
                    goto retry;
                    break;
                default:
                    break;
                }
            }
            fprintf(out, ";\n");
        }
        fprintf(out, "}\n");
    }
}

static void
ecldump_free(ecl_t* ecl)
{
    unsigned int i, j, k;

    if (ecl->anim) {
        for (i = 0; i < ecl->anim_cnt; ++i)
            free(ecl->anim[i]);
        free(ecl->anim);
    }

    if (ecl->ecli) {
        for (i = 0; i < ecl->ecli_cnt; ++i)
            free(ecl->ecli[i]);
        free(ecl->ecli);
    }

    if (ecl->subs) {
        for (i = 0; i < ecl->sub_cnt; ++i) {
            free(ecl->subs[i].name);

            for (j = 0; j < ecl->subs[i].instr_cnt; ++j) {
                free(ecl->subs[i].raw_instrs[j].data);

                if (ecl->subs[i].instrs && ecl->subs[i].instrs[j].params) {
                    for (k = 0; k < ecl->subs[i].instrs[j].param_cnt; ++k) {
                        /* 'o' might store data, but not in ecldump. */
                        if (ecl->subs[i].instrs[j].params[k].type == 's' ||
                            ecl->subs[i].instrs[j].params[k].type == 'c') {
                            free(ecl->subs[i].instrs[j].params[k].value.s.data);
                        }
                    }

                    free(ecl->subs[i].instrs[j].params);
                }
            }

            free(ecl->subs[i].raw_instrs);
            free(ecl->subs[i].instrs);

            for (j = 0; j < ecl->subs[i].label_cnt; ++j)
                free(ecl->subs[i].labels[j].name);

            free(ecl->subs[i].labels);
        }
        free(ecl->subs);
    }
}

int
main(int argc, char* argv[])
{
    int i;
    FILE* f;
    unsigned int mode = ECLDUMP_MODE_NORMAL;
    unsigned int version = 0;
    ecl_t ecl;

    f = stdin;
    current_input = "(stdin)";
    out = stdout;

    argv0 = util_shortname(argv[0]);
    memset(&ecl, 0, sizeof(ecl_t));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit(0);
        } else if (strcmp(argv[i], "-V") == 0) {
            util_print_version("ecldump", PACKAGE_THECL_VERSION);
            exit(0);
        } else if (strcmp(argv[i], "-v") == 0) {
            ++i;
            if (i == argc) {
                print_usage();
                exit(1);
            }
            version = strtol(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "-r") == 0) {
            mode = ECLDUMP_MODE_RAWDUMP;
        } else if (strcmp(argv[i], "-p") == 0) {
            mode = ECLDUMP_MODE_PARAMETERS;
        } else if (strcmp(argv[i], "-o") == 0) {
            ++i;
            if (i == argc) {
                print_usage();
                exit(1);
            }
            out = fopen(argv[i], "w");
            if (!out) {
                fprintf(stderr, "%s: couldn't open %s for writing: %s\n",
                    argv0, argv[i], strerror(errno));
                exit(1);
            }
        } else {
            break;
        }
    }

    if (version != 10 && version != 11 && version != 12) {
        print_usage();
        exit(1);
    }

    if (i != argc) {
        f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "%s: couldn't open %s for reading: %s\n",
                argv0, argv[i], strerror(errno));
            exit(1);
        }

        current_input = argv[i];
    }

    if (open_ecl(&ecl, f) != 0)
        exit(1);

    fclose(f);

    switch (mode) {
    case ECLDUMP_MODE_NORMAL:
        ecldump_translate(&ecl, version);
        ecldump_translate_print(&ecl);
        break;
    case ECLDUMP_MODE_PARAMETERS:
        ecldump_list_params(&ecl);
        break;
    /* This mode might be useful for newer formats. */
    case ECLDUMP_MODE_RAWDUMP:
        ecldump_rawdump(&ecl);
        break;
    }

    ecldump_free(&ecl);
    fclose(out);

    return 0;
}