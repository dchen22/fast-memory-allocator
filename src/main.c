#include <stddef.h>
#include "tlsf.h"

int main(void)
{
    int err = 0;
    if (0 != tlsf_init())
    {
        err = -1;
    }
    if (!err && NULL == tlsf_malloc(50))
    {
        err = -1;
    }
    return err;
}
