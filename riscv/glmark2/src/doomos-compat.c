#include <sys/resource.h>
#include <time.h>

int
getrusage(int who, struct rusage *usage)
{
    (void) who;
    if (!usage)
        return -1;

    usage->ru_utime.tv_sec = 0;
    usage->ru_utime.tv_usec = 0;
    usage->ru_stime.tv_sec = 0;
    usage->ru_stime.tv_usec = 0;
    return 0;
}

time_t
time(time_t *tloc)
{
    if (tloc)
        *tloc = 0;
    return 0;
}
