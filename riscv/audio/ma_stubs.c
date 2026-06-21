/*
 * ma_stubs.c — newlib/SBI link stubs miniaudio references but never executes on
 * bare metal (doc 20 §1.3, doc 13 §8).
 *
 * nanosleep: miniaudio's ma_sleep__posix() is compiled (not gated by
 * MA_NO_THREADING) but never called under MA_NO_THREADING + MA_NO_DEVICE_IO.
 * Provide a no-op so the link resolves. Device-less audio never actually sleeps.
 */
#include <time.h>

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    (void)req;
    (void)rem;
    return 0;
}
