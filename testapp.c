#include <stdio.h>
#include <string.h>
#include <err.h>
#include <blkid/blkid.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct xorshift32_state {
    uint32_t a;
};

/* The state word must be initialized to non-zero */
uint32_t xorshift32(struct xorshift32_state *state)
{
    /* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
    uint32_t x = state->a;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state->a = x;
}

struct xorshift64_state {
    uint64_t a;
};

uint64_t xorshift64(struct xorshift64_state *state)
{
    uint64_t x = state->a;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return state->a = x;
}

struct xorshift128_state {
    uint32_t x[4];
};

/* The state array must be initialized to not be all zero */
uint32_t xorshift128(struct xorshift128_state *state)
{
    /* Algorithm "xor128" from p. 5 of Marsaglia, "Xorshift RNGs" */
    uint32_t t  = state->x[3];

    uint32_t s  = state->x[0];  /* Perform a contrived 32-bit shift. */
    state->x[3] = state->x[2];
    state->x[2] = state->x[1];
    state->x[1] = s;

    t ^= t << 11;
    t ^= t >> 8;
    return state->x[0] = t ^ s ^ (s >> 19);
}

#define SINGLE_BLOCK_SIZE (1024*1024)

uint8_t blocks[SINGLE_BLOCK_SIZE];

int transform_file(const char *in, const char *out, long seed, long numblocks)
{
    int ret = 0;
    long i, j;
    FILE *fin = fopen(in, "rb");
    FILE *fout = fopen(out, "wb");
    struct xorshift32_state state;
    if (fin == NULL || fout == NULL) {
        ret = -1;
        goto finish;
    }

    state.a = seed;

    for(i=0;i<numblocks;i++)
    {
        if (1 != fread(blocks, SINGLE_BLOCK_SIZE, 1, fin)) {
            ret = -1;
            break;
        }

        for(j=0;j<SINGLE_BLOCK_SIZE/sizeof(uint32_t);j++)
        {
            uint32_t dw = xorshift32(&state);
            uint32_t *ptr = (uint32_t *)&blocks[j*sizeof(uint32_t)];
            *ptr ^= dw;
        }

        if (1 != fwrite(blocks, SINGLE_BLOCK_SIZE, 1, fout)) {
            ret = -1;
            break;
        }
    }

finish:
    if (fout != NULL) {
        fclose(fout);
    }

    if (fin != NULL) {
        fclose(fin);
    }
    return ret;
}

int write_file(const char *in, const char *out, long numblocks)
{
    int ret = 0;
    long i, j;
    FILE *fin = fopen(in, "rb");
    FILE *fout = fopen(out, "rb+");
    if (fin == NULL || fout == NULL) {
        ret = -1;
        goto finish;
    }

    for(i=0;i<numblocks;i++)
    {
        if (1 != fread(blocks, SINGLE_BLOCK_SIZE, 1, fin)) {
            ret = -1;
            break;
        }

        if (1 != fwrite(blocks, SINGLE_BLOCK_SIZE, 1, fout)) {
            ret = -1;
            break;
        }
    }

finish:
    if (fout != NULL) {
        fclose(fout);
    }

    if (fin != NULL) {
        fclose(fin);
    }
    return ret;
}

int main (int argc, char *argv[]) {
    const char *blocks_str = NULL;
    const char *seed_str = NULL;
    const char *file_str = NULL;
    long numblocks = 0;
    long seed = 0;
    char input[256];
    blkid_partlist ls;
    int nparts, i;

    int c;
    int fd;
    unsigned long long numblocks2=0;

    while (1)
    {
        static struct option long_options[] =
        {
            {"blocks",  required_argument, 0, 'b'},
            {"seed",  required_argument, 0, 's'},
            {"file",    required_argument, 0, 'f'},
            {0, 0, 0, 0}
        };
        /* getopt_long stores the option index here. */
        int option_index = 0;

        c = getopt_long (argc, argv, "b:s:f:",
                long_options, &option_index);

        /* Detect the end of the options. */
        if (c == -1)
            break;

        switch (c)
        {
            case 0:
                /* If this option set a flag, do nothing else now. */
                if (long_options[option_index].flag != 0)
                    break;
                printf ("option %s", long_options[option_index].name);
                if (optarg)
                    printf (" with arg %s", optarg);
                printf ("\n");
                break;

            case 'b':
                printf ("option -b with value `%s'\n", optarg);
                blocks_str = optarg;
                break;

            case 's':
                printf ("option -s with value `%s'\n", optarg);
                seed_str = optarg;
                break;

            case 'f':
                printf ("option -f with value `%s'\n", optarg);
                file_str = optarg;
                break;

            case '?':
                /* getopt_long already printed an error message. */
                break;

            default:
                abort ();
        }
    }   

    if (blocks_str == NULL)
    {
        printf("Missing blocks\n");
        return -1;
    }

    if (seed_str == NULL)
    {
        printf("Missing seed\n");
        return -1;
    }

    if (file_str == NULL)
    {
        printf("Missing file\n");
        return -1;
    }

    seed = atol(seed_str);
    numblocks = atol(blocks_str);

    blkid_probe pr = blkid_new_probe_from_filename(file_str);
    if (!pr) {
        err(1, "Failed to open %s", file_str);
    }

    ls = blkid_probe_get_partitions(pr);
    if (ls) {
        nparts = blkid_partlist_numof_partitions(ls);
        printf("Number of partitions:%d\n", nparts);
    }
    else {
        printf("Could not get partitions\n");
    }

    if (nparts > 0)
    {
        // Get UUID, label and type
        const char *uuid;
        const char *label;
        const char *type;

        for (i = 0; i < nparts; i++) {
            char dev_name[20];

            sprintf(dev_name, "%s%d", file_str, (i+1));

            pr = blkid_new_probe_from_filename(dev_name);
            blkid_do_probe(pr);

            blkid_probe_lookup_value(pr, "UUID", &uuid, NULL);

            blkid_probe_lookup_value(pr, "LABEL", &label, NULL);

            blkid_probe_lookup_value(pr, "TYPE", &type, NULL);

            printf("Name=%s, UUID=%s, LABEL=%s, TYPE=%s\n", dev_name, uuid, label, type);

        }

        blkid_free_probe(pr);
    }

    FILE *fp = popen("hdparm -I /dev/sdb | grep Model", "r");
    if (fp != NULL)
    {
        char path[256];
        while(fgets(path, sizeof(path) -1,fp) != NULL)
        {
            printf("%s", path);
        }
        pclose(fp);
    }
    else {
        printf("No hdparm output\n");
    }

    fd = open(file_str, O_RDONLY);
    if (fd > 0) {
        ioctl(fd, BLKGETSIZE64, &numblocks2);
        close(fd);
        printf("Number of bytes: %llu, this makes %.3f GB\n",
                numblocks2, 
                (double)numblocks2 / (1024 * 1024 * 1024));
    }
    else
    {
        printf("Cannot display number of blocks\n");
    }

    printf("blocks: %ld\n", numblocks);
    printf("seed: %ld\n", seed);

    printf("To continue type 'yes'\n");
    fgets(input, sizeof(input), stdin);

    if (strstr(input, "yes") != NULL)
    {
        const char *bkpfile_str = "./bkp";
        const char *xorfile_str = "./bkp.xor";
        printf("Starting...\n");
        if (0 != write_file(file_str, bkpfile_str, numblocks))
        {
            printf("Failed to copy initial content\n");
            return -1;
        }
        if (0 != transform_file(bkpfile_str, xorfile_str, seed, numblocks))
        {
            printf("Failed to transform file\n");
            return -1;
        }
        if (0 != write_file(xorfile_str, file_str, numblocks))
        {
            printf("Failed to write final blocks\n");
            return -1;
        }
    }

    return 0;
}
