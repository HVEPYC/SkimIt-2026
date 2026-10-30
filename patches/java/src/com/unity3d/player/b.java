package com.unity3d.player;

import java.io.BufferedReader;
import java.io.FileReader;

/**
 * Stand-in for Unity's CPU probe (obfuscated name {@code b}). The original reads the host's
 * /proc/cpuinfo and refuses to load the engine unless it sees an ARMv6+ CPU with VFP. In the port
 * the engine runs on the runner's emulated ARMv7 CPU (VFPv3 + NEON, like the 2013 devices the game
 * was made for), whatever the phone is, so this reports that CPU: the flags the original computes
 * for "CPU architecture: 7", "Features: ... vfp ... neon vfpv3" and an armeabi-v7a ABI.
 * Memory size still comes from the real /proc/meminfo, as in the original.
 */
final class b {
    // 0x02 ARM, 0x04 v5+, 0x08 v6+, 0x10 v7 (non-"armeabi" ABI), 0x20 vfpv3, 0x40 neon, 0x80 vfp
    private static final int EMULATED_CPU_FLAGS = 0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x40 | 0x80;
    private static final String EMULATED_PROCESSOR = "ARMv7 Processor rev 10 (v7l)";

    private final int memTotalKb;

    b() {
        memTotalKb = readMemTotalKb();
    }

    public final int a() { return EMULATED_CPU_FLAGS; }

    public final String b() { return EMULATED_PROCESSOR; }

    /** Total memory in MB (the original divides /proc/meminfo's kB value by 1024). */
    public final int c() { return memTotalKb / 1024; }

    private static int readMemTotalKb() {
        try (BufferedReader r = new BufferedReader(new FileReader("/proc/meminfo"))) {
            for (String line; (line = r.readLine()) != null; ) {
                if (!line.startsWith("MemTotal")) continue;
                String v = line.substring(line.indexOf(':') + 1).trim();
                int end = 0;
                while (end < v.length() && Character.isDigit(v.charAt(end))) end++;
                return Integer.parseInt(v.substring(0, end));
            }
        } catch (Exception ignored) {
        }
        return 0;
    }
}
