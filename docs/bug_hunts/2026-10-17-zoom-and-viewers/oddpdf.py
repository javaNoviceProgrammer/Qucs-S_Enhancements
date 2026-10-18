import sys
out = sys.argv[1]
def pdf(path, box):
    stream = b"BT /F1 24 Tf 72 700 Td (Hello odd page) Tj ET"
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            b"<< /Type /Page /Parent 2 0 R /MediaBox [" + box.encode() + b"] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
            b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"\nendstream"]
    data = b"%PDF-1.4\n"
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(data))
        data += str(i + 1).encode() + b" 0 obj\n" + o + b"\nendobj\n"
    x = len(data)
    data += b"xref\n0 " + str(len(objs) + 1).encode() + b"\n0000000000 65535 f \n" + b"".join(b"%010d 00000 n \n" % o for o in offs)
    data += b"trailer\n<< /Size " + str(len(objs) + 1).encode() + b" /Root 1 0 R >>\nstartxref\n" + str(x).encode() + b"\n%%EOF\n"
    open(path, "wb").write(data)
for name, box in [("zero", "0 0 0 0"), ("negative", "0 0 -612 -792"), ("flipped", "612 792 0 0"), ("huge", "0 0 1e9 1e9"),
                  ("enormous", "0 0 1e30 1e30"), ("tall", "0 0 612 1e7"), ("wide", "0 0 14400 3"), ("tiny", "0 0 0.01 0.01"),
                  ("nan", "0 0 nan 792"), ("words", "0 0 a b")]:
    pdf(f"{out}/{name}.pdf", box)
