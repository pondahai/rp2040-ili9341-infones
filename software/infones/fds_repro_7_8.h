/*===================================================================*/
/*                                                                   */
/*  fds_repro_7_8.h : temporary instrumentation for fds_plan.md 7.10 */
/*                                                                   */
/*  7.8 fixed the $40xx decode so that cartridge registers above     */
/*  $4017 no longer fold back onto the APU. Two symptoms disappeared  */
/*  with it: 7.4 (background vertical drift in Almana no Kiseki) and  */
/*  7.7 (disk beeping). Only 7.7 has a demonstrated mechanism -- the  */
/*  $4020-$4023 values were seen landing on pulse channel 1. Nothing  */
/*  connects an APU-only corruption to a video symptom, so why 7.4    */
/*  recovered is still unexplained.                                   */
/*                                                                   */
/*  The one thing known to have been happening: $4054 and $4074 are   */
/*  inside the FDS wave RAM ($4040-$407F) and both mask down to       */
/*  0x14, the sprite DMA register. Every wave table load fired a      */
/*  spurious 256-byte SPRRAM copy. That does not obviously match      */
/*  7.4's "sprites looked fine" note, which is exactly why it needs   */
/*  measuring rather than more reasoning.                             */
/*                                                                   */
/*  Setting FDS_REPRO_7_8 to 1 restores the pre-7.8 decode and counts */
/*  how often each mirror of $4014 is hit. Run Almana no Kiseki and   */
/*  watch two things: the per-second counts on the serial log, and    */
/*  whether the vertical drift comes back with them.                  */
/*                                                                   */
/*  This whole file is measurement scaffolding. It is not part of the */
/*  emulator and should be deleted once 7.10 is settled.              */
/*                                                                   */
/*===================================================================*/

#ifndef FDS_REPRO_7_8_H_INCLUDED
#define FDS_REPRO_7_8_H_INCLUDED

/* 1 = reproduce the pre-7.8 fold-back and instrument it.
   0 = normal build, everything below compiles to nothing.

   Deliberately a switch rather than a git revert of 7.8: the measurement
   needs the broken and fixed behaviour toggled back and forth to tell
   correlation from causation, and a branch that defaults to 0 cannot
   reintroduce a fixed high-severity defect if it is merged by mistake. */
#define FDS_REPRO_7_8 0

#if FDS_REPRO_7_8

#include "InfoNES_Types.h"
#include "InfoNES_System.h"

/* Scanlines in roughly one second. Matches FDS_DEBUG_COUNTERS in
   mapper/InfoNES_Mapper_020.cpp. */
#define FDS_REPRO_PERIOD (262 * 60)

/* Hits on each address that masks down to 0x14, indexed by (wAddr >> 5) & 7:
     [0] $4014  the real sprite DMA register
     [1] $4034  FDS disk status
     [2] $4054  inside FDS wave RAM
     [3] $4074  inside FDS wave RAM
     [4] $4094  } above the FDS register block; counted so that a nonzero
     [5] $40B4  } value flags a game reaching somewhere unexpected rather
     [6] $40D4  } than being silently lumped in with the others
     [7] $40F4  }
   Only [0] should ever be nonzero on a correct emulator.

   inline rather than static: K6502_rw.h is included by both K6502.cpp and
   InfoNES_pAPU.cpp while the print lives in InfoNES.cpp, so static would
   give each translation unit its own counters and the log would read zero
   no matter what the game did. C++17 inline variables share one copy. */
inline int FDS_ReproDma[8];

/* Every write above $4017 that reaches the APU decode, and how many of
   those were the sprite DMA case. The ratio says whether the DMA hits are
   a rare corner or a constant background rate. */
inline int FDS_ReproCartWrites;
inline int FDS_ReproTick;

/* Called from the $40xx write path for every address above $4017, before
   the pre-7.8 switch runs. */
static inline void FDS_Repro78_Count(WORD wAddr)
{
  FDS_ReproCartWrites++;
  if ((wAddr & 0x1f) == 0x14)
  {
    FDS_ReproDma[(wAddr >> 5) & 7]++;
  }
}

/* Called once per scanline. Reprints every second rather than dumping
   once, because a USB CDC re-enumeration leaves the host holding a stale
   handle that reads nothing and reports no error (fds_plan.md 7.8). */
static inline void FDS_Repro78_Hsync(void)
{
  if (++FDS_ReproTick < FDS_REPRO_PERIOD)
  {
    return;
  }
  FDS_ReproTick = 0;
  InfoNES_MessageBox(
      "REPRO78 cart/s=%d dma/s 4014=%d 4034=%d 4054=%d 4074=%d "
      "4094=%d 40b4=%d 40d4=%d 40f4=%d",
      FDS_ReproCartWrites, FDS_ReproDma[0], FDS_ReproDma[1],
      FDS_ReproDma[2], FDS_ReproDma[3], FDS_ReproDma[4],
      FDS_ReproDma[5], FDS_ReproDma[6], FDS_ReproDma[7]);
  FDS_ReproCartWrites = 0;
  for (int i = 0; i < 8; i++)
  {
    FDS_ReproDma[i] = 0;
  }
}

#else /* !FDS_REPRO_7_8 */

#define FDS_Repro78_Count(wAddr) ((void)0)
#define FDS_Repro78_Hsync() ((void)0)

#endif /* FDS_REPRO_7_8 */

#endif /* FDS_REPRO_7_8_H_INCLUDED */
