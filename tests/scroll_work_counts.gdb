# Diagnostic call counts only. Debugger timings are not performance results.
set pagination off
set confirm off
set debuginfod enabled off
python
import gdb
import json

counts = {}
active = False

class WorkCounter(gdb.Breakpoint):
    def __init__(self, function, name):
        super().__init__(function, internal=True)
        self.name = name

    def stop(self):
        if active:
            counts[self.name] = counts.get(self.name, 0) + 1
        return False

class Phase(gdb.Breakpoint):
    def stop(self):
        global active, counts
        frame = gdb.newest_frame()
        if int(frame.read_var('phase')):
            counts = {}
            active = True
        else:
            active = False
            scenario = frame.read_var('scenario').string()
            gdb.write('scroll-work,' + json.dumps({'scenario': scenario, **counts}, sort_keys=True) + '\n')
        return False

Phase('ScrollProfileMarker', internal=True)
WorkCounter('clashflux::ui::ScrollRail::Extension::Update', 'rail_updates')
WorkCounter('clashflux::ui::ScrollRail::Extension::PrepareGeometry', 'rail_geometry')
WorkCounter('clashflux::ui::ScrollRail::Extension::OnFrame', 'rail_frames')
WorkCounter('clashflux::ui::ScrollHoverRoute::Refresh', 'hover_refresh')
end
run --scroll-profile
