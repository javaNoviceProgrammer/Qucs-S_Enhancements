#!/usr/bin/env python3
"""A minimal .xlsx whose sheet1.xml body (inside <worksheet>) is argv[2]."""
import sys, zipfile
out, body = sys.argv[1], sys.argv[2].encode()
files = {
 "[Content_Types].xml": b'<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/></Types>',
 "_rels/.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
 "xl/workbook.xml": b'<?xml version="1.0"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="S" sheetId="1" r:id="rId1"/></sheets></workbook>',
 "xl/_rels/workbook.xml.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>',
 "xl/worksheets/sheet1.xml": b'<?xml version="1.0"?><worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">' + body + b'</worksheet>',
}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n, d in files.items():
        z.writestr(n, d)
