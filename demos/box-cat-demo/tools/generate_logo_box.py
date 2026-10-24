"""Derive a complete animated carton while preserving the source logo."""

from copy import deepcopy
from math import cos, sin, pi
from pathlib import Path
import xml.etree.ElementTree as ET

DEMO = Path(__file__).resolve().parents[1]
SOURCE = DEMO.parents[1] / "resources/images/clash_flux_logo_vector_dark_single_seam.svg"
IMAGES = DEMO / "resources/images"
NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", NS)
STYLE = {"fill": "#111318", "stroke": "#e8edf0", "stroke-width": "2.6",
         "stroke-linecap": "round", "stroke-linejoin": "round"}


def save(name, elements):
    svg = ET.Element(f"{{{NS}}}svg", {"viewBox": "0 0 128 128", "width": "128", "height": "128"})
    group = ET.SubElement(svg, f"{{{NS}}}g", STYLE)
    group.extend(deepcopy(elements))
    ET.indent(svg)
    (IMAGES / f"{name}.svg").write_text(ET.tostring(svg, encoding="unicode") + "\n")


def rear_leaf(side):
    # Fold behind the rim; the final face is strongly foreshortened.
    # Matches the complete hinge and open pose in logo_box_geometry.h.
    angle = 220 * pi / 180
    dx = (19.25 if side == 0 else -19.25) * cos(angle)
    dy = 7.1 * cos(angle) - 12 * sin(angle)
    a, b = (64, 49.8), ((25.5, 64) if side == 0 else (102.5, 64))
    points = [a, b, (b[0] + dx, b[1] + dy), (a[0] + dx, a[1] + dy)]
    return ET.Element(f"{{{NS}}}polygon", {
        "id": f"complete-rear-{side}",
        "points": " ".join(f"{x:.5f},{y:.5f}" for x, y in points)})


def main():
    source = ET.parse(SOURCE)
    source.getroot().set("viewBox", "0 0 128 128")
    source.write(IMAGES / "logo.svg", encoding="unicode")
    with (IMAGES / "logo.svg").open("a") as output:
        output.write("\n")
    paths = {element.get("id"): element for element in source.getroot().iter(f"{{{NS}}}path")}
    front = [paths[key] for key in ["box-body-unified", "box-body-outline", "box-center-seam",
                                   "front-left-flap", "front-right-flap"]]
    save("front", front)
    rear = [rear_leaf(0), rear_leaf(1)]
    save("rear", rear)
    interior = [
        ET.Element(f"{{{NS}}}path", {"id": "inner-walls",
            "d": "M25.5 64 L64 49.8 L102.5 64 L64 78.2 Z"}),
        ET.Element(f"{{{NS}}}path", {"id": "inner-corner", "d": "M64 49.8 L64 78.2",
            "fill": "none", "stroke": "#626a75", "stroke-width": "1.3"}),
    ]
    save("interior", interior)
    save("open_logo_box", rear + interior + front)
    # Keep the curled tip but carry the root down inside the carton, where
    # the cat and near flap naturally mask it. This is animation-only art.
    tail = deepcopy(paths["tail"])
    tail.set("d", "M88.5 76.5 C92.2 67.3 90.9 49.3 95.4 43 C99 37.9 103.5 35.6 107.4 36.3 C111.1 37 113.1 40 112.4 43.1 C111.7 46.3 108.7 47.9 105.7 49.1 C101.4 50.8 100.7 56.5 99.5 62.1 C98.4 68.4 96.4 73.9 94 76.8 Q91.1 78 88.5 76.5 Z")
    save("tail", [tail])
    eyes = [element for element in source.getroot().iter(f"{{{NS}}}ellipse")]
    completed = rear + interior + [tail, paths["cat"]] + front
    completed.extend(ET.Element(f"{{{NS}}}ellipse", {
        **eye.attrib, "fill": "#e8edf0", "stroke": "none"}) for eye in eyes)
    save("logo_completed", completed)
    # Closed/open states share the new logo's center joint and rounded base.
    outline = "M26 64.6 L63.2 77.9 Q64 78.2 64.8 77.9 L102.1 64.6 Q102.5 64.1 102.5 65.3 L102.5 96.1 Q102.5 97.8 101 98.6 L64.8 117.5 Q64 117.9 63.2 117.5 L27 98.6 Q25.5 97.8 25.5 96.1 L25.5 65.3 Q25.5 64.1 26 64.6 Z"
    body = [ET.Element(f"{{{NS}}}path", {"id": "closed-box-outline", "d": outline}),
            deepcopy(paths["box-center-seam"])]
    save("body_logo_closed", body)
    top = [ET.Element(f"{{{NS}}}polygon", {"points": points}) for points in [
        "64,49.8 102.5,64 83.25,71.1 44.75,56.9",
        "25.5,64 64,78.2 83.25,71.1 44.75,56.9",
    ]]
    cross_seam = ET.Element(f"{{{NS}}}path", {"id": "lid-cross-seam",
        "d": "M44.75 71.1 L83.25 56.9", "fill": "none"})
    save("closed_logo", body + top + [cross_seam])


if __name__ == "__main__":
    main()
