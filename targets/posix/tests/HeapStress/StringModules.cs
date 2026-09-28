//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

using System;
using System.Text;

namespace HeapStress
{
    // System.String natives: every call below allocates a new string on the managed heap.
    public class StringModule : Module
    {
        public override string Name => "String";

        public override void Run(Harness h, int round)
        {
            string word = "heap" + round;

            h.Equal("heap" + round + "-stress", string.Concat(word, "-", "stress"), "Concat");
            h.Equal("HEAP" + round, word.ToUpper(), "ToUpper");
            h.Equal("heap" + round, ("HEAP" + round).ToLower(), "ToLower");
            h.Equal("ap" + round, word.Substring(2), "Substring");
            h.Equal(2, word.IndexOf("ap"), "IndexOf");
            h.Equal(word.Length + 2, (word + word).LastIndexOf("ap"), "LastIndexOf");
            h.Equal("x", "  x \t".Trim(), "Trim");
            h.Equal("x  ", "  x  ".TrimStart(), "TrimStart");
            h.Equal("0007", "7".PadLeft(4, '0'), "PadLeft");
            h.Equal("7...", "7".PadRight(4, '.'), "PadRight");
            h.Check(word.StartsWith("he") && word.EndsWith(round.ToString()) && word.Contains("eap"), "Starts/Ends/Contains");
            h.Check(string.Compare("abc", "abd") < 0 && "b".CompareTo("a") > 0, "Compare");
            h.Check(string.IsNullOrEmpty("") && !string.IsNullOrEmpty(word), "IsNullOrEmpty");

            string[] parts = ("a,b,,c," + round).Split(',');
            h.Equal(5, parts.Length, "Split count");
            h.Equal(round.ToString(), parts[4], "Split last");

            char[] chars = word.ToCharArray();

            for (int i = 0, j = chars.Length - 1; i < j; i++, j--)
            {
                char c = chars[i];
                chars[i] = chars[j];
                chars[j] = c;
            }

            h.Equal("paeh", new string(chars, chars.Length - 4, 4), "char[] round trip");
            h.Equal("zzzz", new string('z', 4), "new string(char, count)");

            h.Equal("7-x-3.50", string.Format("{0}-{1}-{2:F2}", 7, "x", 3.5), "Format");
            h.Equal("n=" + (round * 3), $"n={round * 3}", "interpolation");

            // Build a long string one piece at a time: every step allocates and drops the previous string.
            string grown = string.Empty;

            for (int i = 0; i < 40 + round * 10; i++)
            {
                grown += (char)('a' + (i % 26));
            }

            h.Equal(40 + round * 10, grown.Length, "grown length");
            h.Check(grown[27] == 'b', "grown content");

            string interned = string.Intern("heapstress-interned");
            h.Check((object)interned == (object)string.IsInterned("heapstress-interned"), "Intern");
        }
    }

    // nanoFramework.System.Text: StringBuilder and UTF-8.
    public class TextModule : Module
    {
        public override string Name => "Text";

        public override void Run(Harness h, int round)
        {
            StringBuilder sb = new StringBuilder();

            for (int i = 0; i < 100 + round * 50; i++)
            {
                sb.Append(i % 10);
            }

            h.Equal(100 + round * 50, sb.Length, "Append length");

            sb.Clear();
            sb.Append("hello world").Insert(5, ",", 1).Replace("world", "heap").Remove(0, 1);
            sb.AppendLine("!");
            h.Equal("ello, heap!\r\n", sb.ToString(), "Insert/Replace/Remove/AppendLine");

            string text = "zażółć gęślą jaźń " + round;
            byte[] utf8 = Encoding.UTF8.GetBytes(text);
            h.Equal(text.Length + 9, utf8.Length, "UTF8 byte count");
            h.Equal(text, new string(Encoding.UTF8.GetChars(utf8)), "UTF8 round trip");
            h.Equal(text, Encoding.UTF8.GetString(utf8, 0, utf8.Length), "UTF8 GetString");
        }
    }

    // Parsing and formatting of numbers, dates, GUIDs and Base64; all of it native.
    public class NumberModule : Module
    {
        public override string Name => "Number";

        public override void Run(Harness h, int round)
        {
            int n = 123456 + round;

            h.Equal(n.ToString(), int.Parse(n.ToString()).ToString(), "int round trip");
            h.Equal("0001E240", 123456.ToString("X8"), "ToString X8");
            h.Equal(-9876543210L, long.Parse("-9876543210"), "long.Parse");
            h.Equal(0xBEEF, Convert.ToInt32("BEEF", 16), "Convert hex");
            h.Near(3.25 + round, double.Parse((3.25 + round).ToString()), 1e-9, "double round trip");
            h.Equal("2.500", 2.5.ToString("F3"), "ToString F3");

            h.Check(int.TryParse("42", out int parsed) && parsed == 42, "TryParse ok");
            h.Check(!int.TryParse("4x2", out parsed), "TryParse bad");

            byte[] raw = BitConverter.GetBytes(0x01020304 + round);
            h.Equal(0x01020304 + round, BitConverter.ToInt32(raw, 0), "BitConverter int");
            h.Near(1.5, BitConverter.ToDouble(BitConverter.GetBytes(1.5), 0), 0, "BitConverter double");

            byte[] data = new byte[37 + round];

            for (int i = 0; i < data.Length; i++)
            {
                data[i] = (byte)(i * 7);
            }

            byte[] back = Convert.FromBase64String(Convert.ToBase64String(data));
            h.Equal(data.Length, back.Length, "Base64 length");
            h.Check(back[data.Length - 1] == data[data.Length - 1], "Base64 content");

            DateTime date = new DateTime(2024, 2, 28, 23, 59, 30).AddSeconds(45 + round);
            h.Equal(29, date.Day, "DateTime.AddSeconds");
            h.Check(date.Month == 2 && date.Hour == 0 && date.Year == 2024, "DateTime fields");
            h.Check(date.ToString().Length > 0 && (date - new DateTime(2024, 2, 29)).TotalSeconds == 15 + round, "DateTime subtract");
            TimeSpan span = TimeSpan.FromMinutes(90 + round) - TimeSpan.FromSeconds(30);
            h.Equal((90 + round) * 60 - 30, (long)span.TotalSeconds, "TimeSpan");
            h.Check(span.ToString().Length > 0, "TimeSpan.ToString");

            Guid guid = Guid.NewGuid();
            h.Check(new Guid(guid.ToString()).Equals(guid) && new Guid(guid.ToByteArray()).Equals(guid), "Guid round trip");

            Random random = new Random(round);
            byte[] noise = new byte[64];
            random.NextBytes(noise);
            int value = random.Next(1000);
            h.Check(value >= 0 && value < 1000 && random.NextDouble() < 1.0, "Random");

            h.Near(12.0, Math.Sqrt(144), 1e-12, "Math.Sqrt");
            h.Near(1024, Math.Pow(2, 10), 1e-9, "Math.Pow");
            h.Near(1.0, Math.Sin(Math.PI / 2), 1e-12, "Math.Sin");
            h.Near(3, Math.Round(2.5 + 0.1), 0, "Math.Round");
            h.Near(-4, Math.Floor(-3.5), 0, "Math.Floor");
            h.Near(Math.PI / 4, Math.Atan2(1, 1), 1e-12, "Math.Atan2");
        }
    }
}
