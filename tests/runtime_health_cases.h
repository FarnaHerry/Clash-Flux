#pragma once
#include "../src/runtime_health.h"
#include <stdexcept>
inline void testRuntimeHealth() {
    using namespace clashflux::health;
    auto require = [](bool value) { if (!value) throw std::runtime_error("runtime health decision regression"); };
    Channel c;
    c.Reset(0);
    auto generation = c.Generation();
    c.Open(generation);
    require(c.Read(9999).state == State::Waiting);
    require(c.Read(10001).state == State::Stale);
    for (Tick time = 1000; time <= 601000; time += 1000) {
        c.Frame(generation, time, true); // valid idle samples
        if (time > 1000) require(c.Read(time).state == State::Fresh);
        require(!c.RepairDue(time));
    }
    c.Consumed();
    c.Frame(generation, 602000, false);
    require(c.Read(608000).state == State::Stale);
    require(c.Read(608000).invalid == 1 && c.Read(608000).valid == c.Read(608000).consumed);
    require(c.RepairDue(608000));
    c.ReserveRepair(608000);
    c.Reopen(608000);
    require(!c.Accepts(generation));
    c.Open(c.Generation());
    require(!c.RepairDue(619000));
    require(c.RepairDue(639000)); c.ReserveRepair(639000);
    require(c.RepairDue(700000)); c.ReserveRepair(700000);
    require(!c.RepairDue(825000)); // three reservations within five minutes
    c.Block(Failure::Authentication);
    require(c.Read(1000000).state == State::Blocked && !c.RepairDue(1000000));
    c.Reset(1000000, false); c.Open(c.Generation());
    require(c.Read(2000000).state == State::Waiting && !c.RepairDue(2000000)); // event silence
    c.Stop(); c.Frame(c.Generation() - 1, 2000001, true);
    require(c.Read(2000001).state == State::Paused && c.Read(2000001).valid == 0);
    c.Reset(0); c.Open(c.Generation()); c.Frame(c.Generation(), 1000, true); c.Frame(c.Generation(), 2000, true);
    c.Resume(600000); require(!c.RepairDue(600001)); // wake grace, no storm
    generation = c.Generation();
    c.Reset(700000);
    require(!c.Accepts(generation));
    c.Frame(generation, 700001, true);
    require(c.Read(700001).valid == 0); // reset must also fence old callbacks
}
