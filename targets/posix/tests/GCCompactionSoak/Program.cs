//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Diagnostics;

namespace GCCompactionSoak
{
    // Long-running twin of GCCompactionBench. Same live-set/fragmentation
    // workload, same LCG, same seed - the only difference is that the
    // iteration count is unbounded by default, so a GC/compaction regression
    // that only shows up after hours of churn has something to show up in.
    //
    // GCCompactionBench exists for short, byte-for-byte A/B comparisons and
    // must stay untouched; this project exists for long unattended runs and
    // must stay a PREFIX of it: running this with MaxIterations set to
    // GCCompactionBench's TotalIterations must reproduce that bench's
    // "GC compaction #" lines exactly.
    //
    // That turned out to be stricter than it sounds. This interpreter's
    // compactor appears to conservatively scan a method's whole local-variable
    // frame for roots, uninitialized slots included, so merely DECLARING an
    // extra local in Main() - even one nothing has assigned yet - shifts the
    // "refs patched" count on every single compaction from the very first
    // one, before that local is ever touched. Extra branches, extra loop
    // conditions and extra CALLS are fine; it is specifically new locals in
    // Main() that break the match. Consequences for this file:
    //   - Main()'s locals are exactly bench's locals, nothing added;
    //   - the internal probe below (needed for parity - see its own comment)
    //     passes its throwaway string straight to a helper call instead of
    //     assigning it anywhere, so it never becomes a new local in Main();
    //   - the human-facing progress heartbeat does the same.
    public class Program
    {
        // ---- Shared with GCCompactionBench - keep byte-for-byte identical ----

        // Live-set slots and size classes. 1500 slots * (1024+4096+16384)/3
        // bytes avg = ~10.25 MB live, inside the 10-14 MB band needed to
        // clear the 32 MB heap's ~8 MB (25%) systematic-GC threshold.
        private const int SlotCount = 1500;
        private static readonly int[] SizeClasses = { 1024, 4096, 16384 };

        // Scattered slots freed-and-reallocated (with the next size class,
        // never the same one) per iteration.
        private const int ChurnPerIteration = 40;

        // GC.Run(true) always compacts, so
        // this cadence IS the compaction cadence.
        private const int CollectEveryIterations = 100;

        // GC.Run(false) still runs a GC pass and, under
        // --compactionaftergc, arms a pending compaction - so, like
        // CollectEveryIterations above, this cadence is part of the
        // compaction sequence, not just telemetry. Must stay at
        // GCCompactionBench's ReportEveryIterations (300): both the cadence
        // AND the exact expression below (string concatenation ahead of the
        // GetTotalMemory call, which allocates a few temporaries of its own)
        // have to match, or the heap layout - and every compaction after this
        // point - drifts. Nothing here is ever printed; see Discard() below.
        private const int CompactionProbeEveryIterations = 300;

        // Fixed-seed LCG (Numerical Recipes constants), identical to
        // GCCompactionBench so the two programs draw the same index sequence.
        private const uint LcgMultiplier = 1664525;
        private const uint LcgIncrement = 1013904223;
        private const uint LcgSeed = 0x2A2A2A2A;

        // ---- Soak-only parameters ----

        // Iteration cap. Negative means unbounded - the default, and the
        // point of this project. Set this to GCCompactionBench's
        // TotalIterations (6000, at the time of writing) to reproduce that
        // bench's exact compaction sequence for the prefix comparison; set it
        // back to a negative value for a real soak before shipping.
        private const int MaxIterations = -1;

        // Progress heartbeat: prints only the iteration number, no GC.* call
        // of any kind, so it can never perturb the compaction sequence above.
        // Measured on the reference host at ~22.6k iterations/second
        // (GCCompactionBench's 6000 iterations in ~0.27s), so ~1.35M
        // iterations/minute; rounded down for headroom on slower hardware.
        private const int ProgressEveryIterations = 1200000;

        private static uint s_lcgState;

        private static byte[][] s_live;
        private static int[] s_liveClass;

        public static void Main()
        {
            s_lcgState = LcgSeed;

            s_live = new byte[SlotCount][];
            s_liveClass = new int[SlotCount];

            // Initial fill: even round-robin across the size classes, kept
            // alive for the whole run by the s_live array reference.
            for (int i = 0; i < SlotCount; i++)
            {
                int cls = i % SizeClasses.Length;
                s_liveClass[i] = cls;
                s_live[i] = new byte[SizeClasses[cls]];
            }

            for (int iter = 0; MaxIterations < 0 || iter < MaxIterations; iter++)
            {
                for (int c = 0; c < ChurnPerIteration; c++)
                {
                    int idx = NextIndex(SlotCount);
                    int newClass = (s_liveClass[idx] + 1) % SizeClasses.Length;

                    // Dropping the old reference here frees that slot's
                    // object; the new array is a different size class, so it
                    // cannot land in the hole just vacated.
                    s_live[idx] = new byte[SizeClasses[newClass]];
                    s_liveClass[idx] = newClass;
                }

                if ((iter + 1) % CollectEveryIterations == 0)
                {
                    nanoFramework.Runtime.Native.GC.Run(true);
                }

                if ((iter + 1) % CompactionProbeEveryIterations == 0)
                {
                    Discard("iter " + (iter + 1) + " freeMemory=" + nanoFramework.Runtime.Native.GC.Run(false));
                }

                if ((iter + 1) % ProgressEveryIterations == 0)
                {
                    Debug.WriteLine("iter " + (iter + 1));
                }
            }

            // Only reachable when MaxIterations >= 0 (the prefix-comparison
            // configuration). A real soak (MaxIterations < 0) never gets here.
            Debug.WriteLine("bench done");
        }

        // Takes the probe's throwaway string as a parameter instead of Main()
        // assigning it to a local - see the class comment for why that
        // distinction matters here.
        private static void Discard(string value)
        {
        }

        private static uint NextLcg()
        {
            s_lcgState = (LcgMultiplier * s_lcgState) + LcgIncrement;
            return s_lcgState;
        }

        private static int NextIndex(int exclusiveMax)
        {
            return (int)(NextLcg() % (uint)exclusiveMax);
        }
    }
}
