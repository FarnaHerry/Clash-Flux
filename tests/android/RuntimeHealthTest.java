package dev.farna.clashflux;

public final class RuntimeHealthTest {
    static void require(boolean value) { if (!value) throw new AssertionError("Android runtime health"); }
    public static void main(String[] args) {
        RuntimeHealth health = new RuntimeHealth();
        health.start(0, 1);
        long generation = health.generation;
        health.connected = true;
        require(health.state(1, false).equals("waiting"));
        for (long now = 1000; now <= 601000; now += 1000) {
            health.sample(generation, now, false); // status fresh even if traffic unavailable
            require(!health.reserve(now, false));
        }
        require(health.state(601000, false).equals("fresh") && !health.trafficAvailable);
        require(health.age(607000) == 6000); // cache reads cannot refresh last status
        require(health.reserve(607000, false));
        long next = health.reopen(607000);
        health.sample(generation, 608000, true); // old client callback rejected
        require(health.age(608000) == 7000 && !health.trafficAvailable);
        health.connected = true;
        require(health.state(608000, false).equals("waiting"));
        health.sample(next, 608000, true);
        require(!health.state(608000, false).equals("fresh"));
        health.sample(next, 609000, true);
        require(health.state(609000, false).equals("fresh"));
        require(!health.reserve(620000, true)); // expected pause
        health.resume(620000);
        require(!health.reserve(620001, false));
        require(health.reserve(640000, false));
        require(!health.reserve(640001, false)); // coalesce multiple Disconnected callbacks
        require(health.reserve(710000, false));
        require(!health.reserve(835000, false)); // window budget
        health.stop();
        health.sample(next, 850000, true);
        require(health.state(850000, false).equals("paused"));
        System.out.println("Android runtime health: idle, unavailable stats, cached status, generation, pause and budget passed");
    }
}
