#!/usr/bin/env python3
"""An .xlsx whose A1..A3 are date-formatted (numFmtId 14) and hold 1e300, nan and -1e300."""
import sys, zipfile
out = sys.argv[1]
NS = 'xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
files = {
 "[Content_Types].xml": b'<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/></Types>',
 "_rels/.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>',
 "xl/workbook.xml": ('<?xml version="1.0"?><workbook %s xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="S" sheetId="1" r:id="rId1"/></sheets></workbook>' % NS).encode(),
 "xl/_rels/workbook.xml.rels": b'<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>',
 "xl/styles.xml": ('<?xml version="1.0"?><styleSheet %s><cellXfs count="2"><xf numFmtId="0"/><xf numFmtId="14"/></cellXfs></styleSheet>' % NS).encode(),
 "xl/worksheets/sheet1.xml": ('<?xml version="1.0"?><worksheet %s><sheetData><row r="1"><c r="A1" s="1"><v>1e300</v></c></row><row r="2"><c r="A2" s="1"><v>nan</v></c></row><row r="3"><c r="A3" s="1"><v>-1e300</v></c></row><row r="4"><c r="A4" s="1"><v>45000.5</v></c></row></sheetData></worksheet>' % NS).encode(),
}
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n, d in files.items():
        z.writestr(n, d)
