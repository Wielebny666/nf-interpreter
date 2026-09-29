//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Threading;

namespace HeapStress
{
    // Runs every module for a fixed number of rounds and checks each result, so the program covers as many native
    // paths of the CLR as it can while the heap is churned by the other modules. It is meant to run on the POSIX host
    // under valgrind with the heap-annotated build and NANOCLR_GC_STRESS set, see targets/posix/doc/HEAP-MEMCHECK.md.
    //
    // The last line printed is either "HEAPSTRESS RESULT: PASS" or "HEAPSTRESS RESULT: FAIL (<n> failed checks)".
    public static class Program
    {
        // Enough for every module to run with the heap in several different states. Under valgrind with
        // NANOCLR_GC_STRESS=1 a round takes minutes, so keep this small.
        private const int Rounds = 5;

        public static void Main()
        {
            Harness harness = new Harness();

            Module[] modules = new Module[]
            {
                new StringModule(),
                new TextModule(),
                new NumberModule(),
                new ArrayModule(),
                new CollectionModule(),
                new ObjectModelModule(),
                new DelegateModule(),
                new ExceptionModule(),
                new ReflectionModule(),
                new ThreadingModule(),
                new TimerModule(),
                new GcModule(),
                new StreamModule(),
                new SerializationModule(),
                new JsonModule(),
                new EventModule(),
                new SocketModule(),
                new RuntimeNativeModule(),
            };

            Console.WriteLine("HEAPSTRESS start: " + modules.Length + " modules, " + Rounds + " rounds");

            for (int round = 0; round < Rounds; round++)
            {
                for (int i = 0; i < modules.Length; i++)
                {
                    Module module = modules[i];

                    harness.Begin(module.Name, round);

                    try
                    {
                        module.Run(harness, round);
                    }
                    catch (Exception ex)
                    {
                        harness.Check(false, "unexpected " + ex.GetType().FullName + ": " + ex.Message);
                    }

                    harness.End();
                }

                // Leave garbage from this round for the next one, but also make sure a full collection with
                // compaction runs between rounds whatever NANOCLR_GC_STRESS is.
                nanoFramework.Runtime.Native.GC.Run(true);

                Console.WriteLine("HEAPSTRESS round " + round + " done, failed checks so far: " + harness.Failures);
            }

            if (harness.Failures == 0)
            {
                Console.WriteLine("HEAPSTRESS RESULT: PASS");
            }
            else
            {
                Console.WriteLine("HEAPSTRESS RESULT: FAIL (" + harness.Failures + " failed checks)");
            }

            // Give the output a moment to reach the host before the CLR exits.
            Thread.Sleep(100);
        }
    }
}
