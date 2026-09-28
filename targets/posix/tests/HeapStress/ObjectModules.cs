//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Collections;

namespace HeapStress
{
    public struct Point
    {
        public int X;
        public int Y;
        public string Label;

        public Point(int x, int y, string label)
        {
            X = x;
            Y = y;
            Label = label;
        }

        public static Point operator +(Point a, Point b) => new Point(a.X + b.X, a.Y + b.Y, a.Label + b.Label);

        public override string ToString() => Label + "(" + X + "," + Y + ")";
    }

    public enum Color
    {
        Red = 1,
        Green = 2,
        Blue = 4,
    }

    // Arrays of every element kind the CLR stores differently: numeric, reference, value type, jagged.
    public class ArrayModule : Module
    {
        public override string Name => "Array";

        public override void Run(Harness h, int round)
        {
            int size = 200 + round * 97;

            int[] numbers = new int[size];

            for (int i = 0; i < size; i++)
            {
                numbers[i] = i * i;
            }

            int[] copy = new int[size + 10];
            Array.Copy(numbers, 0, copy, 5, size);
            h.Equal(numbers[size - 1], copy[size + 4], "Array.Copy");
            Array.Clear(copy, 0, 10);
            h.Equal(0, copy[9], "Array.Clear");
            h.Equal(7, Array.IndexOf(numbers, 49), "Array.IndexOf");

            int[] cloned = (int[])numbers.Clone();
            h.Check(cloned != numbers && cloned[size / 2] == numbers[size / 2], "Clone");

            string[] strings = new string[64];

            for (int i = 0; i < strings.Length; i++)
            {
                strings[i] = "s" + (i + round);
            }

            object[] boxed = new object[strings.Length];
            strings.CopyTo(boxed, 0);
            h.Equal("s" + (63 + round), (string)boxed[63], "CopyTo object[]");

            Point[] points = new Point[32];

            for (int i = 0; i < points.Length; i++)
            {
                points[i] = new Point(i, -i, "p" + i);
            }

            Point sum = points[3] + points[4];
            h.Equal("p3p4(7,-7)", sum.ToString(), "struct array + operator");

            int[][] jagged = new int[10][];

            for (int i = 0; i < jagged.Length; i++)
            {
                jagged[i] = new int[i + round];
            }

            h.Equal(9 + round, jagged[9].Length, "jagged");

            Array created = Array.CreateInstance(typeof(long), 17);
            ((long[])created)[16] = 123L;
            h.Equal(123L, (long)created.GetValue(16), "Array.CreateInstance");

            Color[] colors = new Color[] { Color.Red, Color.Blue };
            h.Check(colors[1] == Color.Blue && (int)colors[0] == 1, "enum array");

            // CoreLibrary 1.17 formats an enum as GetType().GetField("value__").GetValue(this), and on this CLR
            // GetField("value__") returns null.
            string name = null;

            try
            {
                name = colors[1].ToString();
            }
            catch (NullReferenceException)
            {
            }

            h.Known(name == "4" || name == "Blue", "Enum.ToString() throws NullReferenceException");

            byte[] big = new byte[20000 + round * 5000];
            big[big.Length - 1] = 0x5A;
            h.Check(big[0] == 0 && big[big.Length - 1] == 0x5A, "large byte[]");

            int total = 0;

            foreach (int value in numbers)
            {
                total += value & 1;
            }

            h.Equal(size / 2, total, "foreach");
        }
    }

    public class Key
    {
        private readonly int _id;

        public Key(int id)
        {
            _id = id;
        }

        public override int GetHashCode() => _id % 7;

        public override bool Equals(object obj) => obj is Key other && other._id == _id;
    }

    // ArrayList, Hashtable (with colliding custom keys), Queue, Stack and their enumerators.
    public class CollectionModule : Module
    {
        public override string Name => "Collection";

        public override void Run(Harness h, int round)
        {
            int count = 300 + round * 100;

            ArrayList list = new ArrayList();

            for (int i = 0; i < count; i++)
            {
                list.Add(i % 3 == 0 ? (object)("item" + i) : (object)i);
            }

            list.RemoveAt(0);
            list.Insert(0, "first");
            list.Remove(1);
            h.Equal(count - 1, list.Count, "ArrayList count");
            h.Equal("first", (string)list[0], "ArrayList insert");
            h.Check(list.Contains("item3") && list.IndexOf(2) == 1, "ArrayList search");

            object[] asArray = list.ToArray();
            h.Equal(list.Count, asArray.Length, "ArrayList.ToArray");

            Hashtable table = new Hashtable();

            // The native Hashtable compares keys with the CLR's own object equality, not a virtual Equals, so a key
            // object is found only through the instance that was stored; GetHashCode still decides the bucket.
            Key[] keys = new Key[count];

            for (int i = 0; i < count; i++)
            {
                keys[i] = new Key(i);
                table["k" + i] = i;
                table[keys[i]] = "v" + i;
                table[i] = new Point(i, i, null);
            }

            h.Equal(count * 3, table.Count, "Hashtable count");
            h.Equal(count - 1, (int)table["k" + (count - 1)], "Hashtable string key");
            h.Equal("v42", (string)table[keys[42]], "Hashtable colliding key");
            h.Equal(17, ((Point)table[17]).Y, "Hashtable boxed struct");

            for (int i = 0; i < count; i += 2)
            {
                table.Remove("k" + i);
            }

            h.Check(!table.Contains("k0") && table.Contains("k1"), "Hashtable.Remove");

            int entries = 0;

            foreach (DictionaryEntry entry in table)
            {
                if (entry.Key is string)
                {
                    entries++;
                }
            }

            h.Equal(count / 2, entries, "Hashtable enumerate");

            Hashtable clone = (Hashtable)table.Clone();
            table.Clear();
            h.Check(table.Count == 0 && clone.Count > 0, "Hashtable Clone/Clear");

            Queue queue = new Queue();
            Stack stack = new Stack();

            for (int i = 0; i < count; i++)
            {
                queue.Enqueue("q" + i);
                stack.Push(i);
            }

            h.Equal("q0", (string)queue.Dequeue(), "Queue FIFO");
            h.Equal(count - 1, (int)stack.Pop(), "Stack LIFO");
            h.Equal(count - 1, queue.ToArray().Length, "Queue.ToArray");
            h.Equal(count - 2, (int)stack.Peek(), "Stack.Peek");
        }
    }

    public interface IShape
    {
        double Area { get; }

        string Describe();
    }

    public abstract class Shape : IShape
    {
        private static int s_created;

        static Shape()
        {
            s_created = 1000;
        }

        protected Shape()
        {
            s_created++;
        }

        public static int Created => s_created;

        public abstract double Area { get; }

        public virtual string Describe() => GetType().Name + ":" + Area.ToString("F1");
    }

    public class Square : Shape
    {
        private readonly double _side;

        public Square(double side)
        {
            _side = side;
        }

        public override double Area => _side * _side;
    }

    public sealed class Circle : Shape
    {
        private readonly double _radius;

        public Circle(double radius)
        {
            _radius = radius;
        }

        public override double Area => 3.0 * _radius * _radius;

        public override string Describe() => "circle " + base.Describe();
    }

    public class Grid
    {
        private readonly object[] _cells = new object[16];

        public object this[int x, int y]
        {
            get => _cells[y * 4 + x];
            set => _cells[y * 4 + x] = value;
        }
    }

    // Virtual, abstract and interface dispatch, static constructors, boxing, indexers, ref/out and params.
    public class ObjectModelModule : Module
    {
        public override string Name => "ObjectModel";

        public override void Run(Harness h, int round)
        {
            int before = Shape.Created;

            IShape[] shapes = new IShape[20];

            for (int i = 0; i < shapes.Length; i++)
            {
                shapes[i] = i % 2 == 0 ? (IShape)new Square(i + round) : new Circle(i);
            }

            h.Equal(before + 20, Shape.Created, "static field + ctor chain");
            h.Equal("Square:" + (4.0 * (2 + round) * (2 + round) / 4).ToString("F1"), shapes[2].Describe(), "virtual");
            h.Equal("circle Circle:3.0", shapes[1].Describe(), "override + base call");
            h.Check(shapes[3] is Shape && !(shapes[3] is Square), "is");

            object boxed = round + 5;
            h.Equal(round + 5, (int)boxed, "unbox");
            h.Check(boxed.Equals(round + 5) && boxed.GetHashCode() == (round + 5).GetHashCode(), "boxed Equals/GetHashCode");

            object boxedPoint = new Point(1, 2, "b");
            Point unboxed = (Point)boxedPoint;
            unboxed.X = 99;
            h.Equal(1, ((Point)boxedPoint).X, "boxed struct is a copy");

            Grid grid = new Grid();
            grid[1, 2] = "cell";
            grid[3, 3] = round;
            h.Check((string)grid[1, 2] == "cell" && (int)grid[3, 3] == round, "indexer");

            int a = 1;
            Swap(ref a, out int b);
            h.Check(a == 2 && b == 1, "ref/out");

            h.Equal(round + 10, Sum(round, 1, 2, 3, 4), "params");

            Color flags = Color.Red | Color.Blue;
            h.Check(flags.HasFlag(Color.Blue) && !flags.HasFlag(Color.Green), "enum flags");

            h.Throws(typeof(InvalidCastException), () => { string s = (string)boxed; }, "bad cast");
        }

        private static void Swap(ref int a, out int b)
        {
            b = a;
            a = 2;
        }

        private static int Sum(int first, params int[] rest)
        {
            int total = first;

            foreach (int value in rest)
            {
                total += value;
            }

            return total;
        }
    }

    public delegate int Transform(int value);

    public delegate void Notify(string message);

    public class Publisher
    {
        public event Notify Changed;

        public void Raise(string message)
        {
            Notify handler = Changed;

            if (handler != null)
            {
                handler(message);
            }
        }
    }

    // Static, instance and closure delegates, multicast combine/remove and events.
    public class DelegateModule : Module
    {
        public override string Name => "Delegate";

        private int _offset;

        private int AddOffset(int value) => value + _offset;

        private static int Double(int value) => value * 2;

        public override void Run(Harness h, int round)
        {
            _offset = round;

            Transform instance = AddOffset;
            Transform statik = Double;
            int captured = 100 + round;
            Transform closure = v => v + captured;

            h.Equal(10 + round, instance(10), "instance delegate");
            h.Equal(20, statik(10), "static delegate");
            h.Equal(110 + round, closure(10), "closure");

            int calls = 0;
            string last = null;
            Notify first = m => { calls++; last = "1:" + m; };
            Notify second = m => { calls++; last = "2:" + m; };

            Notify both = (Notify)Delegate.Combine(first, second);
            both("x");
            h.Check(calls == 2 && last == "2:x", "multicast");
            h.Equal(2, both.GetInvocationList().Length, "GetInvocationList");

            Notify onlyFirst = (Notify)Delegate.Remove(both, second);
            onlyFirst("y");
            h.Check(calls == 3 && last == "1:y", "Delegate.Remove");

            Publisher publisher = new Publisher();

            for (int i = 0; i < 10; i++)
            {
                int index = i;
                publisher.Changed += m => { calls += index; };
            }

            publisher.Changed += second;
            publisher.Raise("event");
            h.Check(calls == 3 + 45 + 1 && last == "2:event", "event");

            publisher.Changed -= second;
            publisher.Raise("again");
            h.Equal("2:event", last, "event -=");
        }
    }

    public class StressException : Exception
    {
        public int Code { get; }

        public StressException(string message, int code, Exception inner) : base(message, inner)
        {
            Code = code;
        }
    }

    // Exceptions thrown by managed code and by the CLR itself, nested handlers, finally and rethrow.
    public class ExceptionModule : Module
    {
        public override string Name => "Exception";

        public override void Run(Harness h, int round)
        {
            int[] small = new int[3];
            object nothing = null;

            h.Throws(typeof(IndexOutOfRangeException), () => { small[3 + round] = 1; }, "IndexOutOfRange");
            h.Throws(typeof(NullReferenceException), () => { nothing.ToString(); }, "NullReference");
            h.Throws(typeof(StressException), () => { throw new StressException("direct", round, null); }, "custom throw");

            int finallies = 0;

            try
            {
                try
                {
                    Deep(10 + round);
                }
                catch (StressException ex)
                {
                    h.Check(ex.Code == 10 + round && ex.InnerException is InvalidOperationException, "custom exception");
                    throw new StressException("wrapped", -1, ex);
                }
                finally
                {
                    finallies++;
                }
            }
            catch (StressException outer)
            {
                h.Check(outer.Code == -1 && ((StressException)outer.InnerException).Code == 10 + round, "rethrow");
                h.Check(outer.Message == "wrapped", "message");
            }
            finally
            {
                finallies++;
            }

            h.Equal(2, finallies, "finally");

            int caught = 0;

            for (int i = 0; i < 50; i++)
            {
                try
                {
                    throw new InvalidOperationException("loop " + i);
                }
                catch (InvalidOperationException ex)
                {
                    caught += ex.Message.Length > 0 ? 1 : 0;
                }
            }

            h.Equal(50, caught, "throw in loop");
        }

        // Deeper than CLR_RT_Thread::c_MaxStackUnwindDepth from round 3 on, so the CLR has to clip its nested
        // exception records (it prints "TOO MANY NESTED EXCEPTIONS").
        private static void Deep(int depth)
        {
            if (depth == 0)
            {
                throw new InvalidOperationException("bottom");
            }

            try
            {
                Deep(depth - 1);
            }
            catch (InvalidOperationException ex)
            {
                throw new StressException("at depth", depth, ex);
            }
            catch (StressException ex)
            {
                throw new StressException(ex.Message, ex.Code + 1, ex.InnerException);
            }
        }
    }
}
