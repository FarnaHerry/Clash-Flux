package dev.farna.clashflux;

/** Pure monotonic-clock decisions. Owned and serialized by the runtime service. */
final class RuntimeHealth {
    long epoch, generation, sequence, lastStatus = -1, graceUntil, nextAttempt;
    int consecutive, repairs;
    boolean active, connected, trafficAvailable;
    final long[] attempts = {-1, -1, -1};

    void start(long now, long identity) {
        epoch = identity;
        active = true;
        sequence = 0;
        lastStatus = -1;
        trafficAvailable = false;
        repairs = 0;
        nextAttempt = 0;
        java.util.Arrays.fill(attempts, -1);
        reopen(now);
    }
    void stop() { active = false; connected = false; ++generation; }
    void resume(long now) { graceUntil = now + 10000; consecutive = 0; }
    long reopen(long now) { ++generation; connected = false; resume(now); return generation; }
    boolean accepts(long value) { return active && generation == value; }
    void sample(long value, long now, boolean available) {
        if (!accepts(value)) return;
        if (lastStatus < 0 || now - lastStatus > 5000) consecutive = 0;
        lastStatus = now;
        ++sequence;
        ++consecutive;
        trafficAvailable = available;
    }
    long age(long now) { return lastStatus < 0 ? -1 : Math.max(0, now - lastStatus); }
    String state(long now, boolean paused) {
        if (!active || paused) return "paused";
        if (now >= graceUntil && (lastStatus < 0 || now - lastStatus > 5000)) return "stale";
        if (!connected) return "reconnecting";
        return consecutive >= 2 ? "fresh" : "waiting";
    }
    boolean reserve(long now, boolean paused) {
        if (!state(now, paused).equals("stale") || now < nextAttempt) return false;
        int recent = 0;
        for (long at : attempts) if (at >= 0 && now - at < 300000) ++recent;
        if (recent >= 3) return false;
        attempts[repairs % attempts.length] = now;
        ++repairs;
        nextAttempt = now + Math.min(120000L, 30000L << Math.min(2, repairs - 1));
        return true;
    }
}
