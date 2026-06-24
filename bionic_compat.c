/*
 * bionic_compat.c — provide Android Bionic symbols missing from glibc.
 *
 * Build:
 *   gcc -shared -fPIC -o bionic_compat.so bionic_compat.c
 *
 * Use:
 *   LD_PRELOAD=/path/to/bionic_compat.so ./owlet_stream ...
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>

/* Bionic __errno() returns int* to thread-local errno.
 * glibc exposes the same thing via __errno_location(). */
extern int *__errno_location(void);

int *__errno(void)
{
    return __errno_location();
}

/* Bionic __get_h_errno() returns int* to h_errno.
 * glibc: __h_errno_location() */
extern int *__h_errno_location(void);

int *__get_h_errno(void)
{
    return __h_errno_location();
}

/* Bionic __sF[0..2] = {stdin, stdout, stderr} as a FILE* array.
 * TUTK code uses __sF[1] / __sF[2] for stdout/stderr writes.
 * We expose a pointer array initialised at load time. */
FILE *__sF[3];

__attribute__((constructor))
static void init_sF(void)
{
    __sF[0] = stdin;
    __sF[1] = stdout;
    __sF[2] = stderr;
}

/* Bionic FORTIFY __FD_SET_chk(fd, set, setsize) — just delegates. */
void __FD_SET_chk(int fd, fd_set *set, size_t size)
{
    (void)size;
    FD_SET(fd, set);
}

/* Bionic FORTIFY __strlen_chk(s, s_len) */
size_t __strlen_chk(const char *s, size_t s_len)
{
    (void)s_len;
    return strlen(s);
}

/* Bionic FORTIFY __strncpy_chk2 has an extra src_len arg vs glibc's __strncpy_chk */
extern char *__strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len);
char *__strncpy_chk2(char *dst, const char *src, size_t n,
                     size_t dst_len, size_t src_len)
{
    (void)src_len;
    return __strncpy_chk(dst, src, n, dst_len);
}

/* Android Bionic exports res_init; glibc calls it __res_init */
extern int __res_init(void);
int res_init(void)
{
    return __res_init();
}

/*
 * ── getaddrinfo / freeaddrinfo: Bionic vs glibc struct addrinfo mismatch ──
 *
 * Bionic's struct addrinfo swaps the order of ai_canonname and ai_addr
 * relative to glibc:
 *
 *   glibc:                          Bionic (what libIOTCAPIs.so expects):
 *     [24] struct sockaddr *ai_addr   [24] char           *ai_canonname
 *     [32] char       *ai_canonname   [32] struct sockaddr *ai_addr
 *
 * libIOTCAPIs.so (iotc_lookup_host) was built against Bionic headers and
 * reads ai_addr at offset 32. glibc puts NULL ai_canonname there, so the
 * lib dereferences NULL+4 → inet_ntop(src=0x4) → SIGSEGV.
 *
 * Fix: wrap getaddrinfo, call the real glibc one, then deep-copy the result
 * list into Bionic-layout nodes. freeaddrinfo frees our Bionic nodes.
 *
 * hints (input) only uses ai_flags/family/socktype/protocol at offsets
 * 0/4/8/12 — identical in both layouts — so it passes through unchanged.
 */

/* Bionic memory layout of struct addrinfo (48 bytes). */
struct bionic_addrinfo {
    int                     ai_flags;       /* 0  */
    int                     ai_family;      /* 4  */
    int                     ai_socktype;    /* 8  */
    int                     ai_protocol;    /* 12 */
    socklen_t               ai_addrlen;     /* 16 (+4 pad) */
    char                   *ai_canonname;   /* 24 */
    struct sockaddr        *ai_addr;        /* 32 */
    struct bionic_addrinfo *ai_next;        /* 40 */
};

/* Magic stashed just before each node so freeaddrinfo can sanity-check it. */
#define BIONIC_AI_MAGIC 0xB10C0AD1U

typedef int  (*real_getaddrinfo_t)(const char *, const char *,
                                   const struct addrinfo *, struct addrinfo **);
typedef void (*real_freeaddrinfo_t)(struct addrinfo *);

static real_getaddrinfo_t  real_getaddrinfo;
static real_freeaddrinfo_t real_freeaddrinfo;

static void resolve_real_dns(void)
{
    if (!real_getaddrinfo)
        real_getaddrinfo  = (real_getaddrinfo_t)  dlsym(RTLD_NEXT, "getaddrinfo");
    if (!real_freeaddrinfo)
        real_freeaddrinfo = (real_freeaddrinfo_t) dlsym(RTLD_NEXT, "freeaddrinfo");
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    resolve_real_dns();
    if (!real_getaddrinfo)
        return EAI_SYSTEM;

    struct addrinfo *glibc_res = NULL;
    int rc = real_getaddrinfo(node, service, hints, &glibc_res);
    if (rc != 0 || glibc_res == NULL) {
        if (res) *res = NULL;
        return rc;
    }

    struct bionic_addrinfo *head = NULL, *tail = NULL;
    for (struct addrinfo *g = glibc_res; g != NULL; g = g->ai_next) {
        /* Allocate node prefixed with a magic word for freeaddrinfo. */
        uint32_t *blk = malloc(sizeof(uint32_t) + sizeof(struct bionic_addrinfo));
        if (!blk) break;
        *blk = BIONIC_AI_MAGIC;
        struct bionic_addrinfo *b = (struct bionic_addrinfo *)(blk + 1);
        memset(b, 0, sizeof(*b));

        b->ai_flags    = g->ai_flags;
        b->ai_family   = g->ai_family;
        b->ai_socktype = g->ai_socktype;
        b->ai_protocol = g->ai_protocol;
        b->ai_addrlen  = g->ai_addrlen;

        if (g->ai_addr && g->ai_addrlen > 0) {
            b->ai_addr = malloc(g->ai_addrlen);
            if (b->ai_addr)
                memcpy(b->ai_addr, g->ai_addr, g->ai_addrlen);
        }
        if (g->ai_canonname)
            b->ai_canonname = strdup(g->ai_canonname);

        if (!head) head = b;
        else       tail->ai_next = b;
        tail = b;
    }

    if (real_freeaddrinfo)
        real_freeaddrinfo(glibc_res);

    *res = (struct addrinfo *)head;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    struct bionic_addrinfo *b = (struct bionic_addrinfo *)res;
    while (b) {
        struct bionic_addrinfo *next = b->ai_next;
        uint32_t *blk = ((uint32_t *)b) - 1;
        if (*blk == BIONIC_AI_MAGIC) {
            /* one of ours */
            free(b->ai_addr);
            free(b->ai_canonname);
            free(blk);
        } else {
            /* not ours — hand back to glibc (shouldn't normally happen) */
            resolve_real_dns();
            if (real_freeaddrinfo) { real_freeaddrinfo((struct addrinfo *)b); }
            return;
        }
        b = next;
    }
}
