package com.mredis;

import java.util.ArrayList;
import java.util.List;

/**
 * MRedis JNI Wrapper - SHM 기반 In-Memory Store
 * Architecture: Linux SHM + JNI bridge for Java
 * C backend: mredis_core + cmd_dispatch
 */
public class MRedis {

    static {
        System.loadLibrary("mredis");
    }

    private long handlePtr; // native MRedisHandle* as long

    /**
     * Create new MRedis instance
     * @param name SHM name (e.g. "/mredis_test")
     * @param sizeMB size in MB
     */
    public MRedis(String name, long sizeMB) {
        this.handlePtr = nativeCreate(name, sizeMB * 1024 * 1024);
        if (this.handlePtr == 0) {
            throw new RuntimeException("Failed to create MRedis SHM");
        }
    }

    /**
     * Open existing MRedis
     */
    public MRedis(String name) {
        this.handlePtr = nativeOpen(name);
        if (this.handlePtr == 0) {
            throw new RuntimeException("Failed to open MRedis SHM");
        }
    }

    public void close() {
        if (handlePtr != 0) {
            nativeClose(handlePtr);
            handlePtr = 0;
        }
    }

    public void destroy(String name) {
        nativeDestroy(name);
    }

    /**
     * Execute command via cmd_dispatch
     * @param args command arguments as strings
     * @return Reply object
     */
    public Reply exec(String... args) {
        if (handlePtr == 0) {
            throw new IllegalStateException("MRedis not initialized");
        }
        return nativeExec(handlePtr, args);
    }

    // Native methods
    private native long nativeCreate(String name, long size);
    private native long nativeOpen(String name);
    private native void nativeClose(long handle);
    private native void nativeDestroy(String name);
    private native Reply nativeExec(long handle, String[] args);

    // Reply inner class (mirrors s_replyObject)
    public static class Reply {
        public int type; // REPLY_*
        public long integer;
        public double dval;
        public String string;
        public List<Reply> elements;

        public Reply() {
            this.elements = new ArrayList<>();
        }

        @Override
        public String toString() {
            switch (type) {
                case 1: return string != null ? string : "";
                case 3: return String.valueOf(integer);
                case 4: return "(nil)";
                case 5: return "OK";
                case 6: return "ERR: " + (string != null ? string : "");
                case 7: return String.valueOf(dval);
                case 2: // ARRAY
                    StringBuilder sb = new StringBuilder("[");
                    for (int i = 0; i < elements.size(); i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(elements.get(i));
                    }
                    sb.append("]");
                    return sb.toString();
                default: return "Unknown(" + type + ")";
            }
        }
    }
}
