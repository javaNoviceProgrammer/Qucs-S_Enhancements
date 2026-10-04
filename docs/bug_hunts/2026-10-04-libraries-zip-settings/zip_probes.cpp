// The ZIP probes of the 4 October hunt: test functions added to qucs/tests/test_zip_doc.cpp
// for one run each (before itIsExtracted()), then removed. Paths under $H were the scratch folder.
// huntNames and huntSaveNoFlag read archives made in <scratch>/zips by:
//   import zipfile, struct
//   z = zipfile.ZipFile('noflag.zip', 'w'); z.writestr('Résumé.txt', 'x'); z.writestr('日本/a.txt', 'y'); z.close()
//   b = bytearray(open('noflag.zip', 'rb').read())     # the UTF-8 flag (0x800) cleared, as macOS's ditto writes
//   for sig, off in ((b'PK\x01\x02', 8), (b'PK\x03\x04', 6)):
//       i = 0
//       while (j := b.find(sig, i)) >= 0:
//           struct.pack_into('<H', b, j + off, struct.unpack_from('<H', b, j + off)[0] & ~0x800); i = j + 4
//   open('noflag.zip', 'wb').write(b)
//   z = zipfile.ZipFile('fileandfolder.zip', 'w')
//   for n, d in (('a', 'file a'), ('a/x.txt', 'inside'), ('b.txt', 'b'), ('c.txt', 'c')): z.writestr(n, d)
//   z.close()

    void huntDuplicateNames()
    {
        QList<zip::Part> parts;
        for (const char* data : {"old", "new"}) { zip::Part p; p.item.name = "a.txt"; p.data = QByteArray(data); parts << p; }
        const QString file = dir.filePath("dup.zip");
        QVERIFY(write(file, zip::write(parts)));
        ZipDoc doc(nullptr, file);
        QVERIFY(doc.load());
        qWarning() << "HUNT rows:" << shown(&doc) << "names():" << doc.names();
        QString why; bool ok = false;
        qWarning() << "HUNT contents(a.txt):" << doc.contents("a.txt", &why, &ok);
        const QString into = dir.filePath("dupx");
        qWarning() << "HUNT extracted:" << doc.extract({}, into, true, &why) << why << "file:" << read(into + "/a.txt");
        doc.remove({"a.txt"});
        qWarning() << "HUNT after deleting one row:" << doc.names();
    }



    void huntNames()
    {
        const QString H = "<scratch>/zips";
        {
            ZipDoc doc(nullptr, H + "/noflag.zip");
            QVERIFY(doc.load());
            qWarning() << "HUNT noflag names:" << doc.names();
            QString why;
            qWarning() << "HUNT noflag extracted:" << doc.extract({}, dir.filePath("nf"), true, &why) << why;
        }
        {
            ZipDoc doc(nullptr, H + "/fileandfolder.zip");
            QVERIFY(doc.load());
            qWarning() << "HUNT ff rows:" << shown(&doc) << doc.names();
            QString why;
            qWarning() << "HUNT ff extracted:" << doc.extract({}, dir.filePath("ff"), true, &why) << "why:" << why;
            qWarning() << "HUNT ff rename b->c:" << doc.rename("b.txt", "c.txt", &why) << why << doc.names();
            qWarning() << "HUNT ff rename a/x.txt->a (file a exists):" << doc.rename("a/x.txt", "a", &why) << why << doc.names();
        }
    }



    void huntSaveNoFlag()
    {
        ZipDoc doc(nullptr, "<scratch>/zips/noflag.zip");
        QVERIFY(doc.load());
        QString why;
        QVERIFY(doc.makeFolder(QString(), "new", &why));
        qWarning() << "HUNT saved:" << doc.writeTo("<scratch>/zips/noflag-saved.zip");
    }



    void huntManyEntries()
    {
        for (int n : {65534, 65535, 70000}) {
            QList<zip::Part> parts;
            for (int i = 0; i < n; ++i) { zip::Part p; p.item.name = QString::number(i); p.data = "x"; parts << p; }
            const QByteArray bytes = zip::write(parts);
            QString why;
            const auto items = zip::list(bytes, &why);
            qWarning() << "HUNT" << n << "entries written:" << bytes.size() << "bytes; read back:" << items.size() << "why:" << why;
            ZipDoc doc(nullptr, dir.filePath("many.zip"));
            QFile f(dir.filePath("many.zip")); f.open(QIODevice::WriteOnly); f.write(bytes); f.close();
        }
    }



    void huntOddNames()
    {
        QList<zip::Part> parts;
        for (const char* name : {"", ".", "./a.txt", "a//b.txt", "a/./c.txt", "d\\e.txt", "f/", "f", " g.txt", "h.txt ", "con.txt", "a/b.txt"}) {
            zip::Part p; p.item.name = QString::fromLatin1(name); p.data = QByteArray("<") + name + ">"; parts << p;
        }
        const QString file = dir.filePath("odd.zip");
        QVERIFY(write(file, zip::write(parts)));
        ZipDoc doc(nullptr, file);
        QVERIFY(doc.load());
        qWarning() << "HUNT names:" << doc.names();
        qWarning() << "HUNT rows:" << shown(&doc);
        QString why;
        const QString into = dir.filePath("odd");
        const QStringList written = doc.extract({}, into, true, &why);
        qWarning() << "HUNT extracted:" << QStringList(written).replaceInStrings(into, "") << "why:" << why;
        for (const QString& w : written) qWarning() << "HUNT" << QString(w).remove(into) << read(w);
        QString e;
        qWarning() << "HUNT open './a.txt':" << doc.openEntry("./a.txt", &e) << e;
    }


