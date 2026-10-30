package com.unity3d.player;

import java.util.Map;

/**
 * Offline stand-in for Unity 3.5's Java HTTP helper (the game itself never uses WWW).
 *
 * Native code creates one thread per request. Every request fails the way the original did
 * on a device without network access: errorCallback with an "Unable to resolve host" message,
 * then the thread ends (the original also skipped doneCallback on errors). The five static
 * natives keep their original signatures because UnityPlayer.nativeInitWWW registers them.
 */
class WWW extends Thread {
    private final int id;
    private final String url;

    WWW(int id, String url, byte[] postData, Map headers) {
        this.id = id;
        this.url = url;
        start();
    }

    @Override
    public void run() {
        errorCallback(id, "Unable to resolve host \"" + host(url) + "\": No address associated with hostname");
    }

    /** Host part of a URL, parsed by hand so this class has no java.net dependency. */
    private static String host(String url) {
        if (url == null) return "";
        int start = url.indexOf("://");
        start = start < 0 ? 0 : start + 3;
        int end = start;
        while (end < url.length() && "/:?#".indexOf(url.charAt(end)) < 0) end++;
        return url.substring(start, end);
    }

    private static native void doneCallback(int id);

    private static native void errorCallback(int id, String error);

    private static native boolean headerCallback(int id, String headers);

    private static native void progressCallback(int id, float bytesDone, float bytesTotal, double elapsed, int contentLength);

    private static native boolean readCallback(int id, byte[] buffer, int length);
}
