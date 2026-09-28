//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Collections;
using System.Reflection;
using System.Threading;

namespace HeapStress
{
    [AttributeUsage(AttributeTargets.Field)]
    public class MarkerAttribute : Attribute
    {
        public string Tag { get; }

        public MarkerAttribute(string tag)
        {
            Tag = tag;
        }
    }

    public class ReflectionTarget
    {
        [Marker("value")]
        public int Value;

        public string Text;

        public ReflectionTarget()
        {
            Text = "default";
        }

        public ReflectionTarget(int value)
        {
            Value = value;
            Text = "ctor";
        }

        public int Add(int a, int b) => a + b + Value;

        public string Echo(string text, int times)
        {
            string result = string.Empty;

            for (int i = 0; i < times; i++)
            {
                result += text;
            }

            return result;
        }

        public static string Static(object value) => "static:" + value;
    }

    // Type, MethodInfo, FieldInfo and ConstructorInfo: the CLR builds a reflection object for every call.
    public class ReflectionModule : Module
    {
        public override string Name => "Reflection";

        public override void Run(Harness h, int round)
        {
            Type type = typeof(ReflectionTarget);
            ReflectionTarget target = new ReflectionTarget(round);

            MethodInfo[] methods = type.GetMethods();
            int parameters = 0;

            foreach (MethodInfo method in methods)
            {
                parameters += method.GetParameters().Length;
                h.Check(method.Name.Length > 0 && method.DeclaringType != null, "method name/declaring type");
            }

            h.Check(methods.Length >= 4 && parameters >= 5, "GetMethods/GetParameters");

            MethodInfo add = type.GetMethod("Add");
            h.Equal(5 + round, (int)add.Invoke(target, new object[] { 2, 3 }), "MethodInfo.Invoke");
            h.Equal(2, add.GetParameters().Length, "GetParameters count");

            MethodInfo echo = type.GetMethod("Echo", new Type[] { typeof(string), typeof(int) });
            h.Equal("abab", (string)echo.Invoke(target, new object[] { "ab", 2 }), "Invoke with string");

            MethodInfo statik = type.GetMethod("Static", BindingFlags.Public | BindingFlags.Static);
            h.Equal("static:" + round, (string)statik.Invoke(null, new object[] { round }), "static Invoke");

            FieldInfo field = type.GetField("Value");
            field.SetValue(target, 40 + round);
            h.Equal(40 + round, (int)field.GetValue(target), "FieldInfo Set/Get");
            h.Check(field.FieldType == typeof(int), "FieldType");

            object[] attributes = field.GetCustomAttributes(true);
            h.Check(attributes.Length == 1 && ((MarkerAttribute)attributes[0]).Tag == "value", "custom attribute");

            h.Equal(2, type.GetFields().Length, "GetFields");

            ConstructorInfo ctor = type.GetConstructor(new Type[] { typeof(int) });
            ReflectionTarget created = (ReflectionTarget)ctor.Invoke(new object[] { 7 });
            h.Check(created.Value == 7 && created.Text == "ctor", "ConstructorInfo.Invoke");
            h.Equal(2, type.GetConstructors().Length, "GetConstructors");

            Assembly assembly = Assembly.GetExecutingAssembly();
            h.Check(assembly.GetTypes().Length > 20, "Assembly.GetTypes");
            h.Check(assembly.GetType("HeapStress.ReflectionTarget") == type, "Assembly.GetType");
            h.Check(Type.GetType("HeapStress.Square") == typeof(Square), "Type.GetType");
            h.Check(assembly.FullName.IndexOf("HeapStress") >= 0, "Assembly.FullName");

            h.Check(typeof(Square).BaseType == typeof(Shape), "BaseType");
            Type[] interfaces = typeof(Circle).GetInterfaces();
            h.Check(interfaces.Length == 1 && interfaces[0] == typeof(IShape), "GetInterfaces");
            h.Check(typeof(int[]).GetElementType() == typeof(int) && typeof(int[]).IsArray, "array type");
            h.Check(new Square(1).GetType().FullName == "HeapStress.Square", "GetType().FullName");

            // Types as Hashtable keys go through the reflection hash code.
            Hashtable byType = new Hashtable();
            byType[typeof(Square)] = "square";
            byType[typeof(Circle)] = "circle";
            h.Equal("circle", (string)byType[new Circle(round).GetType()], "Type as Hashtable key");
        }
    }

    // Threads that allocate and share data under Monitor, Interlocked, wait handles and cancellation.
    public class ThreadingModule : Module
    {
        public override string Name => "Threading";

        private const int Workers = 4;

        private readonly object _lock = new object();
        private Hashtable _shared;
        private int _counter;

        public override void Run(Harness h, int round)
        {
            _shared = new Hashtable();
            _counter = 0;

            int perWorker = 100 + round * 25;
            Thread[] threads = new Thread[Workers];

            for (int t = 0; t < Workers; t++)
            {
                int id = t;

                threads[t] = new Thread(() =>
                {
                    for (int i = 0; i < perWorker; i++)
                    {
                        string key = "w" + id + "-" + i;
                        byte[] payload = new byte[16 + (i % 64)];

                        lock (_lock)
                        {
                            _shared[key] = payload;
                        }

                        Interlocked.Increment(ref _counter);

                        if (i % 32 == 0)
                        {
                            Thread.Sleep(0);
                        }
                    }
                });

                threads[t].Priority = t % 2 == 0 ? ThreadPriority.Normal : ThreadPriority.BelowNormal;
                threads[t].Start();
            }

            for (int t = 0; t < Workers; t++)
            {
                h.Check(threads[t].Join(60000), "worker " + t + " finished");
            }

            h.Equal(Workers * perWorker, _counter, "Interlocked counter");
            h.Equal(Workers * perWorker, _shared.Count, "shared Hashtable");

            // Ping-pong: two threads hand control back and forth through AutoResetEvents.
            AutoResetEvent ping = new AutoResetEvent(false);
            AutoResetEvent pong = new AutoResetEvent(false);
            int exchanges = 0;

            Thread partner = new Thread(() =>
            {
                for (int i = 0; i < 30; i++)
                {
                    ping.WaitOne();
                    exchanges++;
                    pong.Set();
                }
            });

            partner.Start();

            for (int i = 0; i < 30; i++)
            {
                ping.Set();
                h.Check(pong.WaitOne(30000, false), "pong " + i);
            }

            h.Check(partner.Join(30000) && exchanges == 30, "ping-pong");

            ManualResetEvent[] gates = new ManualResetEvent[] { new ManualResetEvent(false), new ManualResetEvent(false) };
            h.Equal(WaitHandle.WaitTimeout, WaitHandle.WaitAny(gates, 10, false), "WaitAny timeout");

            Thread opener = new Thread(() =>
            {
                Thread.Sleep(5);
                gates[1].Set();
                Thread.Sleep(5);
                gates[0].Set();
            });

            opener.Start();
            h.Equal(1, WaitHandle.WaitAny(gates, 30000, false), "WaitAny");
            h.Check(WaitHandle.WaitAll(gates, 30000, false), "WaitAll");
            opener.Join();

            // A thread that never ends on its own, stopped with Abort.
            Thread endless = new Thread(() =>
            {
                while (true)
                {
                    string garbage = "spin" + Thread.CurrentThread.ManagedThreadId;
                    Thread.Sleep(1);
                }
            });

            endless.Start();
            Thread.Sleep(20);
            endless.Abort();
            h.Check(endless.Join(30000), "Abort");

            CancellationTokenSource cts = new CancellationTokenSource();
            bool callback = false;
            cts.Token.Register(() => { callback = true; });
            cts.Cancel();
            h.Check(cts.IsCancellationRequested && callback, "CancellationToken");
            h.Throws(typeof(OperationCanceledException), () => cts.Token.ThrowIfCancellationRequested(), "ThrowIfCancellationRequested");
        }
    }

    public class TimerModule : Module
    {
        public override string Name => "Timer";

        private int _ticks;

        public override void Run(Harness h, int round)
        {
            _ticks = 0;

            Timer timer = new Timer(state => { Interlocked.Increment(ref _ticks); string s = "tick" + state; }, round, 0, 5);

            WaitFor(() => _ticks >= 5, 30000);
            h.Check(_ticks >= 5, "periodic timer, ticks=" + _ticks);

            timer.Change(Timeout.Infinite, Timeout.Infinite);
            int stopped = _ticks;
            Thread.Sleep(30);
            h.Check(_ticks <= stopped + 1, "Timer.Change stop");

            timer.Change(1, Timeout.Infinite);
            WaitFor(() => _ticks > stopped + 1, 30000);
            h.Check(_ticks > stopped, "one-shot timer");
            timer.Dispose();

            // Timers left to the GC.
            for (int i = 0; i < 10; i++)
            {
                new Timer(state => { }, null, 1000000, Timeout.Infinite);
            }
        }

        internal delegate bool Condition();

        internal static void WaitFor(Condition condition, int timeoutMs)
        {
            DateTime end = DateTime.UtcNow.AddMilliseconds(timeoutMs);

            while (!condition() && DateTime.UtcNow < end)
            {
                Thread.Sleep(5);
            }
        }
    }

    public class Finalizable
    {
        public static int Finalized;
        public static Finalizable Resurrected;

        public readonly string Payload;
        public bool Resurrect;

        public Finalizable(string payload)
        {
            Payload = payload;
        }

        ~Finalizable()
        {
            Interlocked.Increment(ref Finalized);

            if (Resurrect)
            {
                Resurrected = this;
            }
        }
    }

    public class Node
    {
        public Node Next;
        public string Text;
        public int[] Data;
    }

    // Finalizers, resurrection, weak references and survival of a large object graph through compaction.
    public class GcModule : Module
    {
        public override string Name => "GC";

        public override void Run(Harness h, int round)
        {
            Finalizable.Finalized = 0;
            Finalizable.Resurrected = null;

            MakeGarbage(50, round);
            Finalizable suppressed = new Finalizable("suppressed");
            GC.SuppressFinalize(suppressed);

            nanoFramework.Runtime.Native.GC.Run(true);
            GC.WaitForPendingFinalizers();

            h.Check(Finalizable.Finalized >= 50, "finalizers ran: " + Finalizable.Finalized);
            h.Check(Finalizable.Resurrected != null && Finalizable.Resurrected.Payload == "resurrect" + round, "resurrection");

            Finalizable back = Finalizable.Resurrected;
            back.Resurrect = false;
            Finalizable.Resurrected = null;
            GC.ReRegisterForFinalize(back);
            back = null;

            WeakReference weakGarbage = MakeWeak(round);
            string held = "held" + round;
            WeakReference weakHeld = new WeakReference(held);

            nanoFramework.Runtime.Native.GC.Run(true);
            GC.WaitForPendingFinalizers();

            h.Check(!weakGarbage.IsAlive && weakGarbage.Target == null, "WeakReference collected");
            h.Check(weakHeld.IsAlive && (string)weakHeld.Target == held, "WeakReference alive");
            h.Equal("suppressed", suppressed.Payload, "SuppressFinalize object still usable");

            // A long list interleaved with garbage, so compaction has to move most of it.
            Node head = null;
            ArrayList garbage = new ArrayList();
            int count = 1500 + round * 200;

            for (int i = 0; i < count; i++)
            {
                head = new Node { Next = head, Text = "n" + i, Data = new int[i % 17] };
                garbage.Add(new byte[i % 97]);
            }

            garbage = null;
            nanoFramework.Runtime.Native.GC.Run(true);

            int seen = 0;
            bool intact = true;

            for (Node node = head; node != null; node = node.Next)
            {
                int index = count - 1 - seen;
                intact &= node.Text == "n" + index && node.Data.Length == index % 17;
                seen++;
            }

            h.Check(seen == count && intact, "list survived compaction");
        }

        private static void MakeGarbage(int count, int round)
        {
            for (int i = 0; i < count; i++)
            {
                new Finalizable("garbage" + i);
            }

            new Finalizable("resurrect" + round) { Resurrect = true };
        }

        // No finalizer: an object waiting for finalization keeps its weak references alive until the next GC.
        private static WeakReference MakeWeak(int round) => new WeakReference(new Node { Text = "weak" + round });
    }
}
