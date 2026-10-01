/***************************************************************************
    copyright            : (C) 2010 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

#ifndef OCTAVE_WINDOW_H
#define OCTAVE_WINDOW_H

#include <QWidget>
#include <QProcess>
#include <QStringList>

class QEvent;
class QDockWidget;
class QTextEdit;
class QLineEdit;


class OctaveWindow : public QWidget {
  Q_OBJECT
public:
  OctaveWindow(QDockWidget*);
 ~OctaveWindow();

  QSize sizeHint() const;
  bool startOctave();
  /// What it has printed, the commands sent with it.
  QString outputText() const;
  bool isRunning() const;
  void runOctaveScript(const QString&);
  void sendCommand(const QString&);
  void adjustDirectory();

private slots:
  void slotDisplayMsg();
  void slotDisplayErr();
  void slotOctaveEnded(int status);
  void slotSendCommand();

protected:
  bool eventFilter(QObject *obj, QEvent *event);

private:
  QTextEdit *output;
  QProcess octProcess;
  QLineEdit *input;
  QStringList cmdHistory;
  int histPosition;
};

#endif
