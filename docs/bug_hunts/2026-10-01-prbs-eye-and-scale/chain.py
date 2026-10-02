def label(x, y, name):
    return '  <%d %d %d %d "%s" %d %d 0 "">\n' % (x, y, x, y, name, x + 10, y - 20)
def chain(n, extra=''):
    comps = '  <Vdc V1 1 0 60 18 -26 0 1 "5 V" 1>\n  <GND * 1 0 90 0 0 0 0>\n' + extra
    wires = label(0, 30, 'n0')
    for k in range(1, n + 1):
        x = 100 * ((k - 1) % 100) + 100; y = 200 * ((k - 1) // 100)
        comps += '  <R R%d 1 %d %d 15 -26 0 1 "{RV}" 1 "26.85" 0 "0.0" 0 "0.0" 0 "26.85" 0 "european" 0>\n' % (k, x, y + 60)
        wires += label(x, y + 30, 'n%d' % (k - 1))
        if k < n: wires += label(x, y + 90, 'n%d' % k)
    comps += '  <GND * 1 %d %d 0 0 0 0>\n' % (100 * ((n - 1) % 100) + 100, 200 * ((n - 1) // 100) + 90)
    return comps, wires
