/* Copyright holders: Sarah Walker
   see COPYING for more details
*/
/*IBM 5150 cassette nonsense
  Calls F979 twice
  Expects CX to be nonzero, BX >$410 and <$540
    CX is loops between bit 4 of $62 changing
    BX is timer difference between calls
  */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <86box/timer.h>
#include <86box/pit.h>
#include <86box/ppi.h>

PPI ppi;
int ppispeakon;

void
ppi_reset(void)
{
    memset(&ppi, 0x00, sizeof(PPI));
}

#ifdef USE_RMM
#    include <86box/rmm_snap.h>

/* Monitor snapshots: port 61 latch and PIT channel 2 output. */
void
ppi_rmm_snap(rmm_snap_t *s)
{
    RMM_SNAP_VAR(s, ppi);
    RMM_SNAP_VAR(s, ppispeakon);
}
#endif
