//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;

namespace HeapStress
{
    public abstract class Module
    {
        public abstract string Name { get; }

        // One pass over everything the module covers. 'round' varies sizes and values between passes, so each
        // round leaves the heap in a different shape for the modules that follow.
        public abstract void Run(Harness h, int round);
    }

    public class Harness
    {
        // Printing every failure of a broken module would bury the rest of the output.
        private const int MaxReportedPerModule = 5;

        private string _module;
        private int _round;
        private int _moduleFailures;

        public int Failures { get; private set; }

        public void Begin(string module, int round)
        {
            _module = module;
            _round = round;
            _moduleFailures = 0;
        }

        public void End()
        {
            if (_moduleFailures > MaxReportedPerModule)
            {
                Console.WriteLine(
                    "HEAPSTRESS " + _module + " round " + _round + ": " + (_moduleFailures - MaxReportedPerModule) +
                    " more failed checks not shown");
            }
        }

        public void Check(bool condition, string what)
        {
            if (condition)
            {
                return;
            }

            Failures++;
            _moduleFailures++;

            if (_moduleFailures <= MaxReportedPerModule)
            {
                Console.WriteLine("HEAPSTRESS FAIL " + _module + " round " + _round + ": " + what);
            }
        }

        // A check that fails on the current runtime for a reason that is already known and tracked. It is reported but
        // not counted, so the run can still pass; when it starts passing the output says so, and the call should
        // become a plain Check.
        public void Known(bool condition, string issue)
        {
            Console.WriteLine(
                (condition ? "HEAPSTRESS KNOWN ISSUE NOW PASSES " : "HEAPSTRESS KNOWN ") + _module + " round " + _round + ": " +
                issue);
        }

        // Part of a module that the host does not implement.
        public void Skip(string what)
        {
            Console.WriteLine("HEAPSTRESS SKIP " + _module + " round " + _round + ": " + what);
        }

        public void Equal(string expected, string actual, string what)
        {
            Check(expected == actual, what + ": expected '" + expected + "', got '" + actual + "'");
        }

        public void Equal(long expected, long actual, string what)
        {
            Check(expected == actual, what + ": expected " + expected + ", got " + actual);
        }

        public void Near(double expected, double actual, double tolerance, string what)
        {
            double diff = expected - actual;

            if (diff < 0)
            {
                diff = -diff;
            }

            Check(diff <= tolerance, what + ": expected " + expected.ToString() + ", got " + actual.ToString());
        }

        // Runs 'action' and checks that it throws exactly 'expected'.
        public void Throws(Type expected, ThrowingAction action, string what)
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                Check(ex.GetType() == expected, what + ": expected " + expected.Name + ", got " + ex.GetType().Name);
                return;
            }

            Check(false, what + ": expected " + expected.Name + ", nothing thrown");
        }
    }

    public delegate void ThrowingAction();
}
