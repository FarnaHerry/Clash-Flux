"""Generate demo-only static views using the camera in src/box_geometry.h."""

from copy import deepcopy
from math import cos, sin, pi
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1] / "resources" / "images"
NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)
STYLE = {"fill": "#111318", "stroke": "#e8edf0", "stroke-width": "2.6",
         "stroke-linecap": "round", "stroke-linejoin": "round"}


def project(point, front):
    x, y, z = point
    yaw, pitch = (0 if front else -28 * pi / 180), 26 * pi / 180
    horizontal = x * cos(yaw) + y * sin(yaw)
    depth, height = -x * sin(yaw) + y * cos(yaw), z + 0.45
    perspective = 4.5 / (4.5 - depth * cos(pitch) - height * sin(pitch))
    return 64 + 52 * horizontal * perspective, 78 + 52 * (depth * sin(pitch) - height * cos(pitch)) * perspective


def flap(side, opening, front):
    a, b, inward = [
        ((-.5, -.5, 0), (.5, -.5, 0), (0, .5)),
        ((-.5, .5, 0), (.5, .5, 0), (0, -.5)),
        ((-.5, -.5, 0), (-.5, .5, 0), (.5, 0)),
        ((.5, -.5, 0), (.5, .5, 0), (-.5, 0)),
    ][side]
    def free(hinge):
        return project((hinge[0] + inward[0] * cos(opening * pi),
                        hinge[1] + inward[1] * cos(opening * pi),
                        .5 * sin(opening * pi)), front)
    return [project(a, front), project(b, front), free(b), free(a)]


def polygon(points, fill="#111318"):
    return ET.Element(f"{{{NS}}}polygon", {
        "points": " ".join(f"{x:.5f},{y:.5f}" for x, y in points), "fill": fill})


def body(front):
    faces = [[(-.5, .5, 0), (.5, .5, 0), (.5, .5, -1), (-.5, .5, -1)]]
    if not front:
        faces.append([(.5, .5, 0), (.5, -.5, 0), (.5, -.5, -1), (.5, .5, -1)])
    return [polygon([project(p, front) for p in face]) for face in faces]


def save(name, children):
    svg = ET.Element(f"{{{NS}}}svg", {"viewBox": "0 0 128 128", "width": "128", "height": "128"})
    group = ET.SubElement(svg, f"{{{NS}}}g", STYLE)
    group.extend(deepcopy(children))
    ET.indent(svg)
    (ROOT / f"{name}.svg").write_text(ET.tostring(svg, encoding="unicode") + "\n")


def image_children(name):
    return list(ET.parse(ROOT / f"{name}.svg").getroot())


def main():
    for suffix, front in [("perspective", False), ("frontal", True)]:
        rear_sides, near_sides = ([0, 2, 3], [1]) if front else ([0, 2], [3, 1])
        rear = [polygon(flap(side, 1, front), "#1a1e25") for side in rear_sides]
        near = body(front) + [polygon(flap(side, 1, front), "#1a1e25") for side in near_sides]
        save(f"body_{suffix}", body(front))
        save(f"rear_{suffix}", rear)
        save(f"front_{suffix}", near)
        closed = body(front) + [polygon(flap(side, 0, front), "#1a1e25") for side in [2, 3, 0, 1]]
        save(f"closed_{suffix}", closed)
        save(f"logo_{suffix}", image_children("tail") + rear + image_children("cat") + near)


if __name__ == "__main__":
    main()
