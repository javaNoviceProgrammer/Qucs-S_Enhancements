"""Matplotlib's figures in the Python Plots pane of Qucs-S, not windows of
their own: the backend that Run, Debug and the Python Shell are given
(MPLBACKEND=module://_qucs_plots) while Simulation > Python > Plots in Qucs-S
is on.

plt.show() writes each figure open as a picture into the folder QUCS_S_PLOTS
names - with a file about it beside it, written last - and closes it, as a
window closed would have it. A figure's own show(), plt.show(block=False) and
plt.pause() write it and keep it: drawn again, its picture in the pane is
replaced ("update"), as a window's would be - an animation plays in place.
The pane shows them as they come. Run Cell and Run Selection show the
figures they leave open (flush()), as a notebook does.
"""

import itertools
import json
import os
import sys
import time

from matplotlib import _pylab_helpers
from matplotlib.backend_bases import FigureManagerBase
from matplotlib.backends.backend_agg import FigureCanvasAgg

FOLDER = os.environ.get('QUCS_S_PLOTS', '')
SCALE = 2   # pixels a point of the figure's: sharp on a dense screen
_count = itertools.count(1)


def _source():
    """The script the figure is of, or the Python Shell."""
    main = sys.modules.get('__main__')
    path = getattr(main, '__file__', None) or ''
    if not path and sys.argv and sys.argv[0] not in ('', '-c', '-'):
        path = sys.argv[0]
    return os.path.basename(path) if path else 'Python Shell'


def _title(figure):
    texts = []
    suptitle = getattr(figure, '_suptitle', None)
    if suptitle is not None:
        texts.append(suptitle.get_text())
    for axes in figure.axes:
        texts.append(axes.get_title())
    texts.append(figure.get_label() or '')
    return next((str(t) for t in texts if t), '')


def write(figure, number, update=False):
    """The figure as a picture in the pane's folder; False when there is none
    (not inside Qucs-S) or it has nothing drawn. \a update: it stays open,
    and its next picture replaces this one."""
    if not FOLDER or not os.path.isdir(FOLDER) or not figure.axes:
        return False
    stem = '%d-%d-%d' % (os.getpid(), int(time.time() * 1000), next(_count))
    image = os.path.join(FOLDER, stem + '.png')
    part = image + '.part'
    try:
        figure.savefig(part, format='png', dpi=figure.dpi * SCALE, bbox_inches='tight')
        os.replace(part, image)
        about = {'image': stem + '.png', 'source': _source(), 'figure': number, 'title': _title(figure),
                 'time': time.time(), 'scale': SCALE, 'pid': os.getpid(), 'update': bool(update)}
        with open(os.path.join(FOLDER, stem + '.json.part'), 'w', encoding='utf-8') as f:
            json.dump(about, f)
        os.replace(os.path.join(FOLDER, stem + '.json.part'), os.path.join(FOLDER, stem + '.json'))
    except Exception as e:   # (said, as a window that would not open is)
        try:
            os.remove(part)
        except OSError:
            pass
        print('Qucs-S could not show figure %s: %s' % (number, e), file=sys.stderr)
        return False
    return True


class FigureManagerQucs(FigureManagerBase):
    def show(self):
        write(self.canvas.figure, self.num, update=True)


class FigureCanvasQucs(FigureCanvasAgg):
    manager_class = FigureManagerQucs


FigureCanvas = FigureCanvasQucs
FigureManager = FigureManagerQucs


def show(*args, block=None, **kwargs):
    """plt.show(): every figure open written, then closed - kept, with
    block=False (plt.pause() too), to be drawn again."""
    managers = _pylab_helpers.Gcf.get_all_fig_managers()
    keep = block is False
    for manager in managers:
        write(manager.canvas.figure, manager.num, update=keep)
    if keep:
        return
    for manager in managers:
        _pylab_helpers.Gcf.destroy(manager)


def flush():
    """The figures left open, shown (after a cell)."""
    if _pylab_helpers.Gcf.get_all_fig_managers():
        show()
