//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Collections;
using System.IO;
using System.IO.Hashing;
using System.Net;
using System.Net.Sockets;
using System.Runtime.Serialization.Formatters.Binary;
using System.Text;
using System.Threading;
using nanoFramework.Json;
using nanoFramework.Runtime.Events;
using nanoFramework.Runtime.Native;

namespace HeapStress
{
    // MemoryStream, StreamReader/StreamWriter and CRC32.
    public class StreamModule : Module
    {
        public override string Name => "Stream";

        public override void Run(Harness h, int round)
        {
            MemoryStream stream = new MemoryStream();
            int length = 1000 + round * 300;

            for (int i = 0; i < length; i++)
            {
                stream.WriteByte((byte)i);
            }

            byte[] block = new byte[100];
            stream.Seek(500, SeekOrigin.Begin);
            h.Equal(100, stream.Read(block, 0, block.Length), "Read count");
            h.Equal(unchecked((byte)599), block[99], "Read content");
            h.Equal(length, stream.ToArray().Length, "ToArray");

            stream.SetLength(10);
            h.Equal(10, stream.Length, "SetLength");

            MemoryStream text = new MemoryStream();
            StreamWriter writer = new StreamWriter(text);

            for (int i = 0; i < 20 + round; i++)
            {
                writer.WriteLine("line " + i + " żółw");
            }

            writer.Flush();
            text.Seek(0, SeekOrigin.Begin);

            StreamReader reader = new StreamReader(text);
            h.Equal("line 0 żółw", reader.ReadLine(), "ReadLine");
            string rest = reader.ReadToEnd();
            h.Check(rest.IndexOf("line " + (19 + round) + " żółw") >= 0, "ReadToEnd");

            byte[] check = Encoding.UTF8.GetBytes("123456789");
            h.Equal(0xCBF43926, Crc32.HashToUInt32(check), "CRC32 one shot");

            Crc32 crc = new Crc32();
            crc.Append(new byte[] { (byte)'1', (byte)'2', (byte)'3' });
            crc.Append(new SpanByte(check, 3, 6));
            h.Equal(0xCBF43926, crc.GetCurrentHashAsUInt32(), "CRC32 incremental");
        }
    }

    [Serializable]
    public class Payload
    {
        public int Number;
        public string Text;
        public int[] Values;
        public Payload Child;
        public double Ratio;
    }

    // BinaryFormatter walks the object graph with native reflection.
    public class SerializationModule : Module
    {
        public override string Name => "Serialization";

        public override void Run(Harness h, int round)
        {
            Payload original = new Payload
            {
                Number = 42 + round,
                Text = "binary " + round,
                Values = new int[] { 1, 2, 3, round },
                Ratio = 0.25,
                Child = new Payload { Number = -1, Text = "child", Values = new int[0] },
            };

            byte[] bytes;

            try
            {
                bytes = BinaryFormatter.Serialize(original);
            }
            catch (NotImplementedException)
            {
                // targets/posix builds BinaryFormatter_stub.cpp.
                h.Skip("BinaryFormatter is not implemented on this host");
                return;
            }

            h.Check(bytes != null && bytes.Length > 0, "Serialize");

            Payload copy = (Payload)BinaryFormatter.Deserialize(bytes);
            h.Check(copy != null, "Deserialize");

            if (copy != null)
            {
                h.Equal(original.Number, copy.Number, "Number");
                h.Equal(original.Text, copy.Text, "Text");
                h.Check(copy.Values != null && copy.Values.Length == 4 && copy.Values[3] == round, "Values");
                h.Check(copy.Child != null && copy.Child.Text == "child" && copy.Child.Child == null, "Child");
                h.Near(0.25, copy.Ratio, 0, "Ratio");
            }
        }
    }

    public class JsonItem
    {
        public int Id { get; set; }

        public string Name { get; set; }

        public double Price { get; set; }

        public bool Active { get; set; }

        public DateTime Created { get; set; }

        public int[] Tags { get; set; }

        public JsonItem Parent { get; set; }
    }

    // nanoFramework.Json is managed code, but it drives reflection (GetMethods, GetParameters, Invoke) hard.
    public class JsonModule : Module
    {
        public override string Name => "Json";

        public override void Run(Harness h, int round)
        {
            JsonItem item = new JsonItem
            {
                Id = 7 + round,
                Name = "json \"quoted\" " + round,
                Price = 12.5,
                Active = round % 2 == 0,
                Created = new DateTime(2024, 5, 6, 7, 8, 9),
                Tags = new int[] { 3, 1, 4, 1, 5 },
                Parent = new JsonItem { Id = 1, Name = "parent", Tags = new int[0] },
            };

            string json = JsonConvert.SerializeObject(item);
            h.Check(json.IndexOf("\"Id\":" + (7 + round)) >= 0, "SerializeObject: " + json);

            JsonItem back = (JsonItem)JsonConvert.DeserializeObject(json, typeof(JsonItem));
            h.Check(back != null, "DeserializeObject");

            if (back != null)
            {
                h.Equal(item.Id, back.Id, "Id");
                h.Equal(item.Name, back.Name, "Name");
                h.Near(item.Price, back.Price, 1e-9, "Price");
                h.Check(item.Active == back.Active, "Active");
                h.Equal(item.Created.Ticks, back.Created.Ticks, "Created");
                h.Check(back.Tags != null && back.Tags.Length == 5 && back.Tags[4] == 5, "Tags");
                h.Check(back.Parent != null && back.Parent.Name == "parent", "Parent");
            }

            Hashtable table = (Hashtable)JsonConvert.DeserializeObject("{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"e\"}}", typeof(Hashtable));
            h.Check(table != null && table.Count == 3, "Hashtable from JSON");
        }
    }

    // Managed events posted through the native EventSink and WeakDelegate.
    public class EventModule : Module
    {
        public override string Name => "Event";

        private readonly AutoResetEvent _received = new AutoResetEvent(false);
        private uint _data2;

        public override void Run(Harness h, int round)
        {
            CustomEventPostedEventHandler handler = (sender, e) =>
            {
                _data2 = e.Data2;
                _received.Set();
            };

            CustomEvent.CustomEventPosted += handler;

            for (int i = 0; i < 5; i++)
            {
                uint expected = (uint)(round * 100 + i);
                EventSink.PostManagedEvent((byte)EventCategory.Custom, 0, (ushort)i, expected);
                h.Check(_received.WaitOne(30000, false) && _data2 == expected, "custom event " + i);
            }

            CustomEvent.CustomEventPosted -= handler;

            int calls = 0;
            Notify a = m => calls++;
            Notify b = m => calls += 10;
            Notify combined = (Notify)WeakDelegate.Combine(a, b);
            combined("x");
            h.Equal(11, calls, "WeakDelegate.Combine");

            combined = (Notify)WeakDelegate.Remove(combined, a);
            combined("y");
            h.Equal(21, calls, "WeakDelegate.Remove");
        }
    }

    // TCP and UDP over the loopback interface: blocking native calls from several threads.
    public class SocketModule : Module
    {
        public override string Name => "Socket";

        public override void Run(Harness h, int round)
        {
            h.Equal("10.1.2.3", IPAddress.Parse("10.1.2.3").ToString(), "IPAddress.Parse");

            Tcp(h, round);
            Udp(h, round);
        }

        private static int BindLoopback(Socket socket, int fallbackPort)
        {
            socket.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            int port = ((IPEndPoint)socket.LocalEndPoint).Port;

            if (port == 0)
            {
                // Some socket drivers do not report the port they picked.
                socket.Close();
                throw new InvalidOperationException("ephemeral port not reported, use " + fallbackPort);
            }

            return port;
        }

        private static void Tcp(Harness h, int round)
        {
            Socket listener = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            int port = BindLoopback(listener, 0);
            listener.Listen(1);

            int size = 4000 + round * 1000;
            int echoed = 0;

            Thread server = new Thread(() =>
            {
                using (Socket connection = listener.Accept())
                {
                    connection.ReceiveTimeout = 30000;
                    byte[] buffer = new byte[512];

                    while (echoed < size)
                    {
                        int read = connection.Receive(buffer);

                        if (read <= 0)
                        {
                            break;
                        }

                        connection.Send(buffer, 0, read, SocketFlags.None);
                        echoed += read;
                    }
                }
            });

            server.Start();

            byte[] data = new byte[size];

            for (int i = 0; i < size; i++)
            {
                data[i] = (byte)(i * 31 + round);
            }

            byte[] received = new byte[size];
            int total = 0;

            using (Socket client = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
            {
                client.ReceiveTimeout = 30000;
                client.Connect(new IPEndPoint(IPAddress.Loopback, port));
                client.Send(data);

                while (total < size)
                {
                    int read = client.Receive(received, total, size - total, SocketFlags.None);

                    if (read <= 0)
                    {
                        break;
                    }

                    total += read;
                }
            }

            h.Check(server.Join(30000), "TCP server thread finished");
            listener.Close();

            h.Equal(size, total, "TCP echo length");

            bool same = true;

            for (int i = 0; i < size && same; i++)
            {
                same = received[i] == data[i];
            }

            h.Check(same, "TCP echo content");
        }

        private static void Udp(Harness h, int round)
        {
            using (Socket receiver = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            using (Socket sender = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            {
                int port = BindLoopback(receiver, 0);
                receiver.ReceiveTimeout = 30000;

                for (int i = 0; i < 5; i++)
                {
                    byte[] message = Encoding.UTF8.GetBytes("datagram " + round + "/" + i);
                    sender.SendTo(message, new IPEndPoint(IPAddress.Loopback, port));

                    byte[] buffer = new byte[256];
                    EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                    int read = receiver.ReceiveFrom(buffer, ref from);

                    h.Equal("datagram " + round + "/" + i, Encoding.UTF8.GetString(buffer, 0, read), "UDP datagram " + i);
                }
            }
        }
    }

    // nanoFramework.Runtime.Native: system information, GC control and execution constraints.
    public class RuntimeNativeModule : Module
    {
        public override string Name => "RuntimeNative";

        public override void Run(Harness h, int round)
        {
            h.Check(SystemInfo.Version != null && SystemInfo.Version.Major >= 0, "SystemInfo.Version");
            h.Check(SystemInfo.TargetName != null && SystemInfo.Platform != null && SystemInfo.OEMString != null, "SystemInfo strings");

            uint free = nanoFramework.Runtime.Native.GC.Run(false);
            h.Check(free > 0, "GC.Run free bytes");

            ExecutionConstraint.Install(60000, 0);

            string work = string.Empty;

            for (int i = 0; i < 20; i++)
            {
                work += i;
            }

            ExecutionConstraint.Install(-1, 0);
            h.Check(work.Length > 20, "ExecutionConstraint");
        }
    }
}
