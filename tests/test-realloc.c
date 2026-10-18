#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tlsf.h"

static int g_failed;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failed = 1;
    }
}

static int setup(void)
{
    if (0 != tlsf_init())
    {
        fprintf(stderr, "FAIL: tlsf_init\n");
        g_failed = 1;
        return -1;
    }
    return 0;
}

static void test_realloc_same_size_keeps_pointer(void)
{
    if (0 != setup())
    {
        return;
    }

    char *p = tlsf_malloc(50);
    expect(NULL != p, "malloc(50) returns non-NULL");
    if (NULL == p)
    {
        return;
    }
    memset(p, 0xAB, 50);

    char *q = tlsf_realloc(p, 50);
    expect(q == p, "realloc to the same size keeps the pointer");
    if (NULL == q)
    {
        return;
    }
    expect((char)0xAB == q[0] && (char)0xAB == q[49], "payload unchanged");
}

static void test_realloc_grow_in_place(void)
{
    if (0 != setup())
    {
        return;
    }

    char *p = tlsf_malloc(50);
    expect(NULL != p, "malloc(50) returns non-NULL");
    if (NULL == p)
    {
        return;
    }
    memset(p, 0x11, 50);

    /* Right neighbor is the rest of the pool, so this grow can carve it. */
    char *q = tlsf_realloc(p, 200);
    expect(q == p, "grow with a free right neighbor stays in place");
    if (NULL == q)
    {
        return;
    }
    expect((char)0x11 == q[0] && (char)0x11 == q[49], "old bytes kept");
    expect(0 == ((uintptr_t)q & 15), "realloc pointer is 16-byte aligned");
    q[199] = 0x22;
    expect((char)0x22 == q[199], "grown tail is writable");
    expect((char)0x11 == q[49], "writing the tail leaves the old bytes");
}

static void test_realloc_grow_copies_when_blocked(void)
{
    if (0 != setup())
    {
        return;
    }

    char *a = tlsf_malloc(50);
    char *b = tlsf_malloc(50);
    expect(NULL != a && NULL != b, "two malloc(50) succeed");
    if (NULL == a || NULL == b)
    {
        return;
    }
    memset(a, 0x11, 50);
    memset(b, 0x22, 50);

    /* b occupies the right neighbor, so a must be copied to a new block. */
    char *c = tlsf_realloc(a, 200);
    expect(NULL != c, "realloc grow returns non-NULL");
    expect(c != a, "blocked grow returns a new pointer");
    if (NULL == c)
    {
        return;
    }
    expect((char)0x11 == c[0] && (char)0x11 == c[49], "copied bytes match");
    expect((char)0x22 == b[0] && (char)0x22 == b[49], "right neighbor intact");
    expect(0 == ((uintptr_t)c & 15), "copied pointer is 16-byte aligned");

    c[199] = 0x33;
    expect((char)0x33 == c[199] && (char)0x11 == c[49],
           "new tail is writable");

    /* Old block is free again. Reusing it must not clobber the copy or b. */
    char *d = tlsf_malloc(50);
    expect(NULL != d, "malloc reuses the freed block");
    if (NULL != d)
    {
        memset(d, 0x44, 50);
    }
    expect((char)0x11 == c[0] && (char)0x11 == c[49] && (char)0x33 == c[199],
           "copied block survives reuse of the old slot");
    expect((char)0x22 == b[0] && (char)0x22 == b[49],
           "neighbor survives reuse of the old slot");
}

static void test_realloc_shrink_releases_tail(void)
{
    if (0 != setup())
    {
        return;
    }

    char *p = tlsf_malloc(800000);
    expect(NULL != p, "large malloc succeeds");
    if (NULL == p)
    {
        return;
    }
    memset(p, 0x55, 64);

    char *q = tlsf_realloc(p, 64);
    expect(q == p, "shrink keeps the pointer");
    if (NULL == q)
    {
        return;
    }
    expect((char)0x55 == q[0] && (char)0x55 == q[63], "shrunk prefix kept");

    /* The original block occupied most of the pool. A second large malloc
       fits only if the shrink returned the tail to the free list. */
    char *r = tlsf_malloc(700000);
    expect(NULL != r, "large malloc after shrink succeeds");
    expect((char)0x55 == q[0] && (char)0x55 == q[63],
           "large malloc leaves the shrunk block intact");
}

static void test_realloc_oom_leaves_old_block(void)
{
    if (0 != setup())
    {
        return;
    }

    char *p = tlsf_malloc(50);
    expect(NULL != p, "malloc(50) returns non-NULL");
    if (NULL == p)
    {
        return;
    }
    memset(p, 0xAB, 50);

    char *q = tlsf_realloc(p, (uint32_t)1 << 30);
    expect(NULL == q, "realloc larger than the pool returns NULL");
    expect((char)0xAB == p[0] && (char)0xAB == p[49],
           "failed realloc leaves the old block intact");
}

int main(void)
{
    test_realloc_same_size_keeps_pointer();
    test_realloc_grow_in_place();
    test_realloc_grow_copies_when_blocked();
    test_realloc_shrink_releases_tail();
    test_realloc_oom_leaves_old_block();

    if (0 != g_failed)
    {
        fprintf(stderr, "some tests failed\n");
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
