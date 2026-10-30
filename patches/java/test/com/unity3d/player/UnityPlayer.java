package com.unity3d.player;

import android.app.Activity;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Test double for Unity's player: records UnitySendMessage calls with their time. */
public class UnityPlayer {
    public static Activity currentActivity;

    public static final List<String[]> MESSAGES = Collections.synchronizedList(new ArrayList<>());
    public static volatile long startNanos = System.nanoTime();

    public static void UnitySendMessage(String gameObject, String method, String message) {
        long ms = (System.nanoTime() - startNanos) / 1_000_000;
        MESSAGES.add(new String[] {gameObject, method, message, Long.toString(ms)});
    }
}
