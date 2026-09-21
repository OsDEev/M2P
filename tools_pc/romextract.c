// melee_romextract: GameCube ISO -> directory tree extractor.
//
// The PC port reads game files from an extracted disc root (see --disc),
// so a plain-ISO dump must be unpacked first:
//
//   melee_romextract game.iso out/        # full tree extract
//   melee_romextract --list game.iso      # print file tree + sizes
//   melee_romextract --one game.iso internal/path.dat out.dat
//
// Format notes: plain ISO/GCM layout (0x440-byte header, FST at the
// offset/size in the header, big-endian). Compressed formats (CISO/GCZ/
// RVZ/WIA) are detected and rejected with a hint to convert to plain ISO
// first (e.g. Dolphin → right-click the game → Convert → ISO).
//
// Portable C99, no dependencies, 64-bit file offsets throughout.

#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#if defined(_MSC_VER) && !defined(fseeko)
// MSVC has no POSIX fseeko/ftello (MinGW does).
#define fseeko _fseeki64
#define ftello _ftelli64
typedef __int64 off_t;
#endif
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(p) mkdir(p, 0755)
#define PATH_SEP '/'
#endif

#define HDR_GAMEID 0x00
#define HDR_MAGIC 0x1C
#define HDR_FST_OFF 0x424
#define HDR_FST_SIZE 0x428
#define GC_MAGIC 0xC2339F3Dul

#define CHUNK (1u << 20)

static uint32_t rb32(const uint8_t* p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) | p[3];
}

static uint32_t rb24(const uint8_t* p)
{
    return ((uint32_t) p[0] << 16) | ((uint32_t) p[1] << 8) | p[2];
}

typedef struct {
    FILE* f;
    uint8_t* fst;
    uint32_t fst_size;
    uint32_t nentries;
    const char* strings;
    char game_id[7];
    uint64_t files_done;
    uint64_t bytes_done;
} Iso;

static int iso_open(Iso* iso, const char* path)
{
    uint8_t hdr[0x440];
    uint32_t fst_off, fst_size;
    memset(iso, 0, sizeof(*iso));
    iso->f = fopen(path, "rb");
    if (!iso->f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 0;
    }
    if (fread(hdr, 1, sizeof(hdr), iso->f) != sizeof(hdr)) {
        fprintf(stderr, "error: '%s' too small for a GC disc image\n",
                path);
        fclose(iso->f);
        return 0;
    }
    // compressed-format detection (all unsupported, with hints)
    if (!memcmp(hdr, "CISO", 4) || !memcmp(hdr, "\xB1\x0B\xC0\x01", 4) ||
        !memcmp(hdr, "RVZ\0", 4) || !memcmp(hdr, "WIA\0", 4)) {
        fprintf(stderr,
                "error: '%s' is a compressed image (CISO/GCZ/RVZ/WIA).\n"
                "Convert it to a plain ISO first (Dolphin: right-click "
                "the game -> Convert -> ISO).\n",
                path);
        fclose(iso->f);
        return 0;
    }
    memcpy(iso->game_id, hdr + HDR_GAMEID, 6);
    iso->game_id[6] = '\0';
    if (rb32(hdr + HDR_MAGIC) != GC_MAGIC) {
        fprintf(stderr,
                "warning: bad GameCube magic (not a GC/Wii disc image?)\n");
    }
    fst_off = rb32(hdr + HDR_FST_OFF);
    fst_size = rb32(hdr + HDR_FST_SIZE);
    if (!fst_off || !fst_size || fst_size > (64u << 20)) {
        fprintf(stderr, "error: bogus FST offset/size (%u/%u)\n", fst_off,
                fst_size);
        fclose(iso->f);
        return 0;
    }
    iso->fst = (uint8_t*) malloc(fst_size);
    if (!iso->fst) {
        fprintf(stderr, "error: out of memory\n");
        fclose(iso->f);
        return 0;
    }
    if (fseeko(iso->f, fst_off, SEEK_SET) != 0 ||
        fread(iso->fst, 1, fst_size, iso->f) != fst_size) {
        fprintf(stderr, "error: cannot read FST\n");
        free(iso->fst);
        fclose(iso->f);
        return 0;
    }
    iso->fst_size = fst_size;
    iso->nentries = rb32(iso->fst + 8); // root "next" = entry count
    if (iso->nentries < 1 || iso->nentries * 12u > fst_size) {
        fprintf(stderr, "error: bogus FST entry count (%u)\n",
                iso->nentries);
        free(iso->fst);
        fclose(iso->f);
        return 0;
    }
    iso->strings = (const char*) (iso->fst + iso->nentries * 12u);
    return 1;
}

static void iso_close(Iso* iso)
{
    if (iso->f)
        fclose(iso->f);
    free(iso->fst);
    memset(iso, 0, sizeof(*iso));
}

// entry accessors (all big-endian on disc)
static int ent_is_dir(const Iso* iso, uint32_t i)
{
    return iso->fst[i * 12u] != 0;
}

static const char* ent_name(const Iso* iso, uint32_t i)
{
    uint32_t off = rb24(iso->fst + i * 12u + 1);
    const char* base = iso->strings;
    size_t max = iso->fst_size - (size_t) (base - (const char*) iso->fst);
    if (off >= max)
        return "?";
    return base + off;
}

static uint32_t ent_off(const Iso* iso, uint32_t i)
{
    return rb32(iso->fst + i * 12u + 4);
}

static uint32_t ent_len(const Iso* iso, uint32_t i)
{
    return rb32(iso->fst + i * 12u + 8);
}

static void list_tree(const Iso* iso, uint32_t dir, int depth,
                      uint64_t* total)
{
    // children of dir live in [dir+1, next(dir))
    uint32_t next = ent_is_dir(iso, dir) ? ent_len(iso, dir) : dir + 1;
    uint32_t c;
    int k;
    for (k = 0; k < depth; k++)
        printf("  ");
    printf("[%s]\n", dir == 0 ? "." : ent_name(iso, dir));
    for (c = dir + 1; c < next && c < iso->nentries;) {
        if (ent_is_dir(iso, c)) {
            uint32_t nx = ent_len(iso, c);
            if (nx <= c + 1) {
                c++;
                continue; // empty dir
            }
            list_tree(iso, c, depth + 1, total);
            c = nx;
        } else {
            for (k = 0; k < depth + 1; k++)
                printf("  ");
            printf("%s (%u bytes)\n", ent_name(iso, c),
                   ent_len(iso, c));
            *total += ent_len(iso, c);
            c++;
        }
    }
}

static int make_dirs(const char* path)
{
    // mkdir -p for the directory portion of path
    char tmp[4096];
    size_t i, n;
    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    {
        const char* bs = strrchr(path, '\\');
        if (bs && (!slash || bs > slash))
            slash = bs;
    }
#endif
    if (!slash)
        return 1;
    n = (size_t) (slash - path);
    if (n >= sizeof(tmp))
        return 0;
    memcpy(tmp, path, n);
    tmp[n] = '\0';
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char save = tmp[i];
            tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = save;
        }
    }
    MKDIR(tmp);
    return 1;
}

static int extract_one(Iso* iso, uint32_t idx, const char* outpath,
                       int verbose)
{
    static uint8_t buf[CHUNK];
    uint64_t off = ent_off(iso, idx);
    uint64_t left = ent_len(iso, idx);
    FILE* out;
    if (!make_dirs(outpath)) {
        fprintf(stderr, "error: cannot create dirs for '%s'\n", outpath);
        return 0;
    }
    out = fopen(outpath, "wb");
    if (!out) {
        fprintf(stderr, "error: cannot write '%s'\n", outpath);
        return 0;
    }
    if (fseeko(iso->f, (off_t) off, SEEK_SET) != 0) {
        fprintf(stderr, "error: seek failed\n");
        fclose(out);
        return 0;
    }
    while (left > 0) {
        size_t want = left > CHUNK ? CHUNK : (size_t) left;
        size_t got = fread(buf, 1, want, iso->f);
        if (!got) {
            fprintf(stderr, "error: short read in image\n");
            fclose(out);
            return 0;
        }
        if (fwrite(buf, 1, got, out) != got) {
            fprintf(stderr, "error: short write to '%s'\n", outpath);
            fclose(out);
            return 0;
        }
        left -= got;
        iso->bytes_done += got;
    }
    fclose(out);
    iso->files_done++;
    if (verbose) {
        printf("  %s (%u bytes)\n", outpath, ent_len(iso, idx));
        fflush(stdout);
    } else if ((iso->files_done & 63) == 0) {
        printf("\r%llu files, %llu MB...", (unsigned long long) iso->files_done,
               (unsigned long long) (iso->bytes_done >> 20));
        fflush(stdout);
    }
    return 1;
}

static int extract_tree(Iso* iso, uint32_t dir, const char* outdir,
                        const char* prefix, int verbose)
{
    // children of dir d live in [d+1, next(d)); nested dirs recurse.
    char child[4096], sub[4096];
    uint32_t next = ent_len(iso, dir);
    uint32_t c;
    for (c = dir + 1; c < next && c < iso->nentries;) {
        if (ent_is_dir(iso, c)) {
            uint32_t nx = ent_len(iso, c);
            if (nx <= c + 1) {
                c++;
                continue; // empty dir
            }
            snprintf(sub, sizeof(sub), "%s%s/", prefix,
                     ent_name(iso, c));
            if (!extract_tree(iso, c, outdir, sub, verbose))
                return 0;
            c = nx;
        } else {
            snprintf(child, sizeof(child), "%s/%s%s", outdir, prefix,
                     ent_name(iso, c));
            if (!extract_one(iso, c, child, verbose))
                return 0;
            c++;
        }
    }
    return 1;
}

static uint32_t find_path(Iso* iso, const char* path)
{
    // internal path like "audio/us/xxx.hps" (leading '/' tolerated)
    char tmp[1024];
    const char* toks[64];
    int ntok = 0, ti;
    uint32_t dir = 0;
    while (*path == '/')
        path++;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    {
        char* p = tmp;
        char* tok_start = NULL;
        for (; *p; p++) {
            if (*p == '\\')
                *p = '/';
            if (*p == '/') {
                *p = '\0';
                if (tok_start && ntok < 64)
                    toks[ntok++] = tok_start;
                tok_start = NULL;
            } else if (!tok_start) {
                tok_start = p;
            }
        }
        if (tok_start && ntok < 64)
            toks[ntok++] = tok_start;
    }
    if (!ntok)
        return 0xFFFFFFFFu;
    for (ti = 0; ti < ntok; ti++) {
        uint32_t next = ent_len(iso, dir);
        uint32_t c;
        int last = (ti == ntok - 1);
        int found = 0;
        for (c = dir + 1; c < next && c < iso->nentries;) {
            if (ent_is_dir(iso, c)) {
                uint32_t nx = ent_len(iso, c);
                if (!last && !strcmp(ent_name(iso, c), toks[ti])) {
                    dir = c;
                    found = 1;
                    break;
                }
                c = (nx > c) ? nx : c + 1;
            } else {
                if (last && !strcmp(ent_name(iso, c), toks[ti]))
                    return c;
                c++;
            }
        }
        if (!found)
            return 0xFFFFFFFFu;
    }
    return 0xFFFFFFFFu; // path ended on a directory
}

static void usage(const char* prog)
{
    printf("usage:\n"
           "  %s game.iso out/             extract full tree\n"
           "  %s --list game.iso            list files\n"
           "  %s --one game.iso in/iso/path.dat out.dat\n",
           prog, prog, prog);
}

int main(int argc, char** argv)
{
    Iso iso;
    const char* iso_path = NULL;
    const char* out_arg = NULL;
    const char* one_in = NULL;
    int list_only = 0;
    int ai;
    for (ai = 1; ai < argc; ai++) {
        if (!strcmp(argv[ai], "--list")) {
            list_only = 1;
        } else if (!strcmp(argv[ai], "--one")) {
            if (ai + 2 >= argc) {
                usage(argv[0]);
                return 2;
            }
            one_in = argv[++ai];
            out_arg = argv[++ai];
        } else if (!iso_path) {
            iso_path = argv[ai];
        } else if (!out_arg && !list_only) {
            out_arg = argv[ai];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!iso_path || (!list_only && !one_in && !out_arg)) {
        usage(argv[0]);
        return 2;
    }
    if (!iso_open(&iso, iso_path))
        return 1;
    printf("game id: %s, %u files\n", iso.game_id, iso.nentries - 1);
    if (list_only) {
        uint64_t total = 0;
        list_tree(&iso, 0, 0, &total);
        printf("total: %llu bytes in %u files\n",
               (unsigned long long) total, iso.nentries - 1);
        iso_close(&iso);
        return 0;
    }
    if (one_in) {
        uint32_t idx = find_path(&iso, one_in);
        int ok;
        if (idx == 0xFFFFFFFFu || ent_is_dir(&iso, idx)) {
            fprintf(stderr, "error: '%s' not found in image\n", one_in);
            iso_close(&iso);
            return 1;
        }
        ok = extract_one(&iso, idx, out_arg, 1);
        iso_close(&iso);
        return ok ? 0 : 1;
    }
    {
        char outdir[4096];
        size_t n = strlen(out_arg);
        if (n >= sizeof(outdir)) {
            fprintf(stderr, "error: output path too long\n");
            iso_close(&iso);
            return 1;
        }
        memcpy(outdir, out_arg, n + 1);
        while (n > 0 && (outdir[n - 1] == '/' || outdir[n - 1] == '\\'))
            outdir[--n] = '\0';
        if (!extract_tree(&iso, 0, outdir, "", 0)) {
            iso_close(&iso);
            return 1;
        }
        printf("\rdone: %llu files, %llu MB\n",
               (unsigned long long) iso.files_done,
               (unsigned long long) (iso.bytes_done >> 20));
    }
    iso_close(&iso);
    return 0;
}
