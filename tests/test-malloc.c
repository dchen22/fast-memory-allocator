#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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

static void test_malloc_write_aligned(void)
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
    expect(0 == ((uintptr_t)p & 15), "malloc pointer is 16-byte aligned");
    memset(p, 0xAB, 50);
    expect((char)0xAB == p[0] && (char)0xAB == p[49], "payload is writable");
}

static void test_free_null(void)
{
    if (0 != setup())
    {
        return;
    }
    tlsf_free(NULL);
}

static void test_two_allocs_distinct(void)
{
    if (0 != setup())
    {
        return;
    }

    char *a = tlsf_malloc(50);
    char *b = tlsf_malloc(50);
    expect(NULL != a && NULL != b, "two malloc(50) succeed");
    expect(a != b, "two mallocs return distinct pointers");
    if (NULL == a || NULL == b)
    {
        return;
    }

    memset(a, 0x11, 50);
    memset(b, 0x22, 50);
    expect((char)0x11 == a[0] && (char)0x11 == a[49], "first payload intact");
    expect((char)0x22 == b[0] && (char)0x22 == b[49], "second payload intact");
}

static void test_free_then_malloc_does_not_clobber_other(void)
{
    if (0 != setup())
    {
        return;
    }

    char *a = tlsf_malloc(50);
    char *b = tlsf_malloc(50);
    expect(NULL != a && NULL != b, "setup two blocks");
    if (NULL == a || NULL == b)
    {
        return;
    }
    memset(b, 0x22, 50);
    tlsf_free(a);
    char *c = tlsf_malloc(50);
    expect(NULL != c, "malloc after free succeeds");
    if (NULL != c)
    {
        memset(c, 0x33, 50);
    }
    expect((char)0x22 == b[0] && (char)0x22 == b[49],
           "live neighbor not clobbered");
}

static void test_free_both_then_reuse(void)
{
    if (0 != setup())
    {
        return;
    }

    void *a = tlsf_malloc(50);
    void *b = tlsf_malloc(50);
    expect(NULL != a && NULL != b, "setup two blocks");
    tlsf_free(a);
    tlsf_free(b);
    void *c = tlsf_malloc(50);
    expect(NULL != c, "malloc after freeing both succeeds");
}

static void test_oom(void)
{
    if (0 != setup())
    {
        return;
    }

    void *p = tlsf_malloc((uint32_t)1 << 30);
    expect(NULL == p, "malloc larger than pool returns NULL");
}

static void test_fill_free_all_then_large(void)
{
    if (0 != setup())
    {
        return;
    }

    enum { kMax = 20000, kSlot = 128 };
    void **ptrs = malloc(sizeof(*ptrs) * kMax);
    expect(NULL != ptrs, "libc malloc for pointer list");
    if (NULL == ptrs)
    {
        return;
    }

    int n = 0;
    while (n < kMax)
    {
        void *p = tlsf_malloc(kSlot);
        if (NULL == p)
        {
            break;
        }
        memset(p, 0x5A, kSlot);
        ptrs[n++] = p;
    }
    expect(n > 2, "filled pool with more than two blocks");

    for (int i = 0; i < n; i++)
    {
        tlsf_free(ptrs[i]);
    }

    /* One slot cannot hold this; coalescing the whole pool must. */
    void *big = tlsf_malloc((uint32_t)kSlot * (uint32_t)(n / 2));
    expect(NULL != big, "large malloc after free-all (coalesce)");

    free(ptrs);
}

int main(void)
{
    test_malloc_write_aligned();
    test_free_null();
    test_two_allocs_distinct();
    test_free_then_malloc_does_not_clobber_other();
    test_free_both_then_reuse();
    test_oom();
    test_fill_free_all_then_large();

    if (0 != g_failed)
    {
        fprintf(stderr, "some tests failed\n");
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
