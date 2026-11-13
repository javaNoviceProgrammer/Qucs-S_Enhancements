"""Phase 7 of RF 2.5D's accuracy limits: an open-stub bandstop filter of our own, its dimensions chosen here.
A 50 Ohm microstrip on 254 um of alumina (eps_r 9.8) over a ground plane, lossless, with three pairs of open
stubs - one either side of the line, so that the filter is symmetric about its axis -, each stub 50 Ohm and a quarter
wave at 10 GHz less its open end (Hammerstad and Jensen: w 247 um, eps_eff 6.564, lambda_g/4 2925 um; the open end
80 um), the pairs a quarter wave apart, 1.5 mm of feed at each end. In micrometres, x along the line, y across."""
H, EPS = 254.0, 9.8
W = 250.0            # the line's width and each stub's
STUB = 2850.0        # each stub, from the line's edge
PITCH = 2925.0       # between the pairs' centres
FEED = 1500.0        # from each end to the nearest pair's centre
CENTRES = [FEED + k * PITCH for k in range(3)]
LENGTH = 2 * FEED + 2 * PITCH

def outline(half=False):
    """The filter as one polygon (half: its part at y >= 0, closed along the axis)."""
    top = [(0.0, W / 2)]
    for c in CENTRES:
        top += [(c - W / 2, W / 2), (c - W / 2, W / 2 + STUB), (c + W / 2, W / 2 + STUB), (c + W / 2, W / 2)]
    top.append((LENGTH, W / 2))
    if half:
        return [(0.0, 0.0), (LENGTH, 0.0)] + top[::-1]
    return top + [(x, -y) for x, y in top[::-1]]


CROSS = 3000.0       # one pair alone: a line this long, the pair at its middle

def cross(half=False):
    """One pair of stubs alone, at the middle of a line CROSS long - the filter's junction and stubs by themselves."""
    c = CROSS / 2
    top = [(0.0, W / 2), (c - W / 2, W / 2), (c - W / 2, W / 2 + STUB), (c + W / 2, W / 2 + STUB), (c + W / 2, W / 2), (CROSS, W / 2)]
    if half:
        return [(0.0, 0.0), (CROSS, 0.0)] + top[::-1]
    return top + [(x, -y) for x, y in top[::-1]]


def line(half=False):
    """The line alone, CROSS long: what the ports present, with nothing on it."""
    return [(0.0, 0.0 if half else -W / 2), (CROSS, 0.0 if half else -W / 2), (CROSS, W / 2), (0.0, W / 2)]
