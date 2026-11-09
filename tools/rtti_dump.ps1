param(
  [string]$Exe = "Z:\SteamLibrary\steamapps\common\DARK SOULS REMASTERED\DarkSoulsRemastered.exe",
  [string]$Out = "C:\Users\treyf\OneDrive\PTDE Modding\mctde-Link\tools\dsr_vtables.txt"
)

$src = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;

public static class Rtti
{
    [DllImport("dbghelp.dll", CharSet=CharSet.Ansi, SetLastError=true)]
    static extern uint UnDecorateSymbolName(string name, StringBuilder outStr, uint maxLen, uint flags);

    struct Section { public uint VA; public uint VSize; public uint Raw; public uint RawSize; public string Name; }
    static List<Section> secs = new List<Section>();
    static byte[] data;
    static ulong imageBase;
    static Dictionary<string,string> demCache = new Dictionary<string,string>();

    static uint RU32(int off){ return (uint)(data[off] | data[off+1]<<8 | data[off+2]<<16 | data[off+3]<<24); }
    static ulong RU64(int off){ return (ulong)RU32(off) | ((ulong)RU32(off+4) << 32); }

    static int RvaToOff(uint rva){
        foreach(var s in secs){
            if(rva >= s.VA && rva < s.VA + Math.Max(s.VSize, s.RawSize)){
                uint d = rva - s.VA;
                if(d < s.RawSize) return (int)(s.Raw + d);
                return -1;
            }
        }
        return -1;
    }
    static bool RvaValid(uint rva){ return RvaToOff(rva) >= 0; }

    static string ReadCString(int off){
        var sb = new StringBuilder();
        int max = off + 8192;
        for(int i=off; i<data.Length && i<max; i++){ if(data[i]==0) break; sb.Append((char)data[i]); }
        return sb.ToString();
    }
    static string ReadCStringFixed(int off, int len){
        var sb = new StringBuilder();
        for(int i=0;i<len;i++){ byte c=data[off+i]; if(c==0) break; sb.Append((char)c); }
        return sb.ToString();
    }

    // Canonical RTTI demangle: type descriptor name ".?AVFoo@@" -> "??_R0?AVFoo@@@8" -> "Foo"
    static string Demangle(string td){
        if(string.IsNullOrEmpty(td) || td.Length < 4 || td[0] != '.') return td;
        string cached;
        if(demCache.TryGetValue(td, out cached)) return cached;
        string outName;
        try {
            string dec = "??_R0" + td.Substring(1) + "@8";
            var sb = new StringBuilder(8192);
            uint r = UnDecorateSymbolName(dec, sb, 8192, 0);
            if(r==0){ outName = td; }
            else {
                string s = sb.ToString();
                int i = s.IndexOf(" `RTTI Type Descriptor'");
                if(i>=0) s = s.Substring(0,i);
                s = s.Trim();
                // drop leading "class "/"struct "/"union "/"enum "
                foreach(var pre in new string[]{"class ","struct ","union ","enum "}){
                    if(s.StartsWith(pre)){ s = s.Substring(pre.Length); break; }
                }
                outName = s.Length>0 ? s : td;
            }
        } catch { outName = td; }
        demCache[td] = outName;
        return outName;
    }

    // top-level namespace for grouping (text before first "::", ignoring template args)
    static string TopNs(string name){
        int depth=0;
        for(int i=0;i<name.Length;i++){
            char c=name[i];
            if(c=='<') depth++;
            else if(c=='>') depth--;
            else if(depth==0 && c==':' && i+1<name.Length && name[i+1]==':') return name.Substring(0,i);
        }
        return "(global)";
    }

    public static void Run(string path, string outPath){
        data = File.ReadAllBytes(path);
        int e_lfanew = (int)RU32(0x3C);
        int coff = e_lfanew + 4;
        int numSec = data[coff+2] | data[coff+3]<<8;
        int optSize = data[coff+16] | data[coff+17]<<8;
        int opt = coff + 20;
        ushort magic = (ushort)(data[opt] | data[opt+1]<<8);
        if(magic != 0x20B) throw new Exception("Not PE32+ (x64).");
        imageBase = RU64(opt + 24);
        int secTable = opt + optSize;
        for(int i=0;i<numSec;i++){
            int b = secTable + i*40;
            var s = new Section();
            s.Name = ReadCStringFixed(b, 8);
            s.VSize = RU32(b+8); s.VA = RU32(b+12); s.RawSize = RU32(b+16); s.Raw = RU32(b+20);
            secs.Add(s);
        }

        // Pass 1: Complete Object Locators (x64) in .rdata/.data
        var colByVA = new Dictionary<ulong, uint[]>(); // colVA -> {typeDescRva, subobjOffset}
        foreach(var s in secs){
            if(!(s.Name.StartsWith(".rdata") || s.Name.StartsWith(".data"))) continue;
            int start = (int)s.Raw;
            int end = (int)(s.Raw + s.RawSize) - 24;
            for(int off=start; off<=end; off+=4){
                if(RU32(off) != 1) continue;
                uint colRva = s.VA + (uint)(off - s.Raw);
                if(RU32(off+20) != colRva) continue;
                uint pType = RU32(off+12), pHier = RU32(off+16);
                if(!RvaValid(pType) || !RvaValid(pHier)) continue;
                if(RvaToOff(pType) < 0) continue;
                colByVA[imageBase + colRva] = new uint[]{ pType, RU32(off+4) };
            }
        }

        // Pass 2: .rdata 8-byte values == COL VA  =>  vtable starts at value+8
        var results = new List<object[]>(); // {vtableVA(ulong), subobjOffset(uint), name(string)}
        foreach(var s in secs){
            if(!s.Name.StartsWith(".rdata")) continue;
            int start = (int)s.Raw;
            int end = (int)(s.Raw + s.RawSize) - 8;
            for(int off=start; off<=end; off+=8){
                ulong v = RU64(off);
                uint[] col;
                if(!colByVA.TryGetValue(v, out col)) continue;
                uint vtRva = (s.VA + (uint)(off - s.Raw)) + 8;
                int tdOff = RvaToOff(col[0]);
                string raw = (tdOff>=0) ? ReadCString(tdOff + 16) : "";
                results.Add(new object[]{ imageBase + vtRva, col[1], Demangle(raw) });
            }
        }

        // group by top-level namespace
        var groups = new Dictionary<string, List<object[]>>();
        foreach(var r in results){
            string ns = TopNs((string)r[2]);
            if(!groups.ContainsKey(ns)) groups[ns] = new List<object[]>();
            groups[ns].Add(r);
        }
        var nsNames = new List<string>(groups.Keys);
        nsNames.Sort(StringComparer.Ordinal);

        var sb2 = new StringBuilder();
        sb2.AppendLine("# DarkSoulsRemastered.exe  RTTI vtable dump");
        sb2.AppendLine("# ImageBase = 0x" + imageBase.ToString("X"));
        sb2.AppendLine("# RVA = VA - ImageBase. Columns: VA  [+subobject_offset]  class");
        sb2.AppendLine("# Vtables: " + results.Count + "   Namespaces: " + nsNames.Count);
        sb2.AppendLine();
        // table of contents
        sb2.AppendLine("=== Namespaces (count) ===");
        foreach(var ns in nsNames) sb2.AppendLine(string.Format("  {0,-28} {1}", ns, groups[ns].Count));
        sb2.AppendLine();

        foreach(var ns in nsNames){
            var list = groups[ns];
            list.Sort((a,b)=>{
                int c = string.Compare((string)a[2],(string)b[2],StringComparison.Ordinal);
                if(c!=0) return c;
                return ((ulong)a[0]).CompareTo((ulong)b[0]);
            });
            sb2.AppendLine("################################################################");
            sb2.AppendLine("## " + ns + "   (" + list.Count + ")");
            sb2.AppendLine("################################################################");
            foreach(var r in list){
                ulong va = (ulong)r[0]; uint so = (uint)r[1]; string name=(string)r[2];
                string off = so==0 ? "" : ("  +"+so);
                sb2.AppendLine("0x" + va.ToString("X") + "\t" + name + off);
            }
            sb2.AppendLine();
        }
        File.WriteAllText(outPath, sb2.ToString());
        Console.WriteLine("ImageBase=0x"+imageBase.ToString("X"));
        Console.WriteLine("Vtables="+results.Count+"  Namespaces="+nsNames.Count);
        Console.WriteLine("Output: "+outPath);
    }
}
'@

Add-Type -TypeDefinition $src -Language CSharp
[Rtti]::Run($Exe, $Out)
