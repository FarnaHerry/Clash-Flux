"""Independent minimal MMDB writer for isolated production download tests."""
import ipaddress


def mmdb_file(country="CN", *, asn=None, width=28):
    def value(kind, body=b"", count=None):
        size = len(body) if count is None else count
        assert size < 285
        return bytes([((kind if kind < 8 else 0) << 5) | min(size, 29)]) + (
            bytes([kind - 7]) if kind >= 8 else b"") + (
            bytes([size - 29]) if size >= 29 else b"") + body

    def text(body):
        return value(2, body.encode())

    def integer(kind, number):
        return value(kind, number.to_bytes(max(1, (number.bit_length() + 7) // 8), "big"))

    def mapping(fields):
        return value(7, b"".join(text(key) + data for key, data in fields.items()), len(fields))

    nodes = [[None, None]]
    for network in ("::7f00:2/128", "2001:db8::/32"):
        net = ipaddress.ip_network(network)
        node = 0
        for bit in range(net.prefixlen):
            side = (int(net.network_address) >> (127 - bit)) & 1
            if bit == net.prefixlen - 1:
                nodes[node][side] = "data"
            else:
                if nodes[node][side] is None:
                    nodes[node][side] = len(nodes)
                    nodes.append([None, None])
                node = nodes[node][side]
    count = len(nodes)
    output = bytearray()
    for node in nodes:
        left, right = [count if child is None else count + 16 if child == "data" else child for child in node]
        if width == 28:
            output += (left & 0xffffff).to_bytes(3, "big")
            output += bytes([((left >> 24) << 4) | (right >> 24)])
            output += (right & 0xffffff).to_bytes(3, "big")
        else:
            output += left.to_bytes(width // 8, "big") + right.to_bytes(width // 8, "big")
    record = mapping({"autonomous_system_number": integer(6, asn)}) if asn else mapping({"country": mapping({"iso_code": text(country)})})
    return bytes(output) + bytes(16) + record + b"\xab\xcd\xefMaxMind.com" + mapping({
        "node_count": integer(6, count), "record_size": integer(5, width),
        "ip_version": integer(5, 6), "database_type": text("GeoLite2-ASN" if asn else "GeoLite2-Country"),
        "binary_format_major_version": integer(5, 2), "binary_format_minor_version": integer(5, 0),
        "build_epoch": integer(9, 1),
    })
