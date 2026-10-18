import random, sys
out = sys.argv[1]
rnd = random.Random(7)
def write(path, n, spaced):
    parts = []
    for i in range(n):
        x, y = rnd.uniform(20, 590), rnd.uniform(20, 770)
        ch = chr(ord('a') + i % 26)
        parts.append(f"BT /F1 {rnd.choice([4, 6, 8])} Tf {x:.1f} {y:.1f} Td ({ch}{' ' if spaced else ''}) Tj ET")
    stream = "\n".join(parts).encode()
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
            b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream"]
    data = b"%PDF-1.4\n"; offs = []
    for i, o in enumerate(objs):
        offs.append(len(data)); data += str(i + 1).encode() + b" 0 obj\n" + o + b"\nendobj\n"
    x = len(data)
    data += b"xref\n0 " + str(len(objs) + 1).encode() + b"\n0000000000 65535 f \n" + b"".join(b"%010d 00000 n \n" % o for o in offs)
    data += b"trailer\n<< /Size " + str(len(objs) + 1).encode() + b" /Root 1 0 R >>\nstartxref\n" + str(x).encode() + b"\n%%EOF\n"
    open(path, "wb").write(data)
write(f"{out}/scatter3000.pdf", 3000, False)
write(f"{out}/scatter3000_spaces.pdf", 3000, True)
write(f"{out}/scatter12000.pdf", 12000, False)
