#!/usr/bin/env python3
"""Undoes each piece of the Background Jobs, one at a time; its tests must fail.

    python3 breaks.py <build tree> [name ...]

Each break is a text replaced in a source file. The tree is built with it and
test_claude_jobs and test_mcp_server's theLongToolsAreJobs and
theProgramServesOverStdio run, offscreen; a break that nothing fails on is said
to have survived. The sources are put back whatever happens.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "..", "qucs-s-26.1.1", "qucs")

BREAKS = [
    ("a stop says stopped at once", "claudejobs.cpp",
     "        job.status = Status::Stopping;\n        job.stopAsked = true;",
     "        job.status = Status::Stopped;\n        job.stopAsked = true;"),
    ("a stop with no end stays stopping for ever", "claudejobs.cpp",
     "        if (e == nullptr || e->stopSerial != serial || e->job.status != Status::Stopping) return;",
     "        return;"),
    ("a program's ending says stopped", "claudejobs.cpp",
     "            job.status = Status::Lost;\n            job.said = why;",
     "            job.status = Status::Stopped;\n            job.said = why;"),
    ("clear finished takes what runs", "claudejobs.cpp",
     "        if (isOver(a_entries.at(i).job.status)) {\n            gone << a_entries.at(i).job.id;",
     "        if (true) {\n            gone << a_entries.at(i).job.id;"),
    ("a percentage made up", "claudejobs.cpp",
     'return isOver(job.status) ? QStringLiteral("-") : tr("unknown");',
     'return QStringLiteral("0 %");'),
    ("an end can be changed later", "claudejobs.cpp",
     "        job.status = before.status;\n        job.ended = before.ended;",
     ""),
    ("no percentage bound", "claudejobs.cpp",
     "    job.percent = std::clamp(job.percent, -1, 100);\n",
     ""),
    ("the list has no end", "claudejobs.cpp",
     "    while (a_entries.size() > kMost) {",
     "    while (false) {"),
    ("the oldest goes first, running or not", "claudejobs.cpp",
     "            if (isOver(a_entries.at(i).job.status)) {\n                at = i;",
     "            if (false) {\n                at = i;"),
    ("ambient tasks listed", "claudejobs.cpp",
     'if (m.value(QLatin1String("ambient")).toBool() && id.isEmpty()) return;',
     ""),
    ("a task's kept running across a run", "claudejobs.cpp",
     "        if (!isOver(job.status)) job.status = Status::Lost;\n",
     ""),
    ("a pipe opened as the kept file", "claudejobs.cpp",
     "if (!misc::isPlainFile(a_kept) || info.size() > kKeptMost) return;",
     ""),
    ("started never becomes running", "claudejobs.cpp",
     "if (job.status == Status::Started) job.status = Status::Running;\n        });\n    if (!going)",
     "});\n    if (!going)"),
    ("stop_task not written", "claudecode.cpp",
     '{QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("stop_task")},',
     '{QStringLiteral("request"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("no_stop_task")},'),
    ("a refused stop not said", "claudecode.cpp",
     "            registry.stopRefused(registry.taskJob(a_caller, task), response.value(QLatin1String(\"error\")).toString());\n",
     ""),
    ("a closed session's tasks left running", "claudecode.cpp",
     'jobs::Registry::instance().programEnded(a_caller, tr("Its conversation was closed: Claude Code ended without saying how this ended."));',
     ""),
    ("no menu item", "claudecodepanel.cpp",
     'jobs->setObjectName(QStringLiteral("claudeShowBackgroundJobs"));',
     ""),
    ("a new window each time", "claudejobswindow.cpp",
     "    if (!g_window) {\n        // (Above",
     "    {\n        // (Above"),
    ("a long name widens the window", "claudejobswindow.cpp",
     "    header->setSectionResizeMode(Name, QHeaderView::Stretch);",
     "    header->setSectionResizeMode(Name, QHeaderView::ResizeToContents);\n    a_table->setMinimumWidth(1400);"),
    ("a long summary widens the window", "claudejobswindow.cpp",
     "    a_summary->setWordWrap(true);\n",
     ""),
    ("the server reads the window's jobs", "mcpstdio.cpp",
     "    qucs_s::claude::jobs::Registry::instance().reset();\n",
     ""),
    ("the bar full for an unknown", "claudejobswindow.cpp",
     "        bar->setRange(0, 0);\n",
     "        bar->setRange(0, 100);\n        bar->setValue(100);\n"),
    ("Stop on for what cannot stop", "claudejobswindow.cpp",
     "stop->setEnabled(job.canStop && job.status != Status::Stopping && !isOver(job.status));",
     "stop->setEnabled(true);"),
    ("a job over keeps its Stop", "claudejobswindow.cpp",
     "stop->setVisible(!isOver(job.status));",
     ""),
    ("the tools' job not told it was answered", "qucscontrol.cpp",
     "        jobAnswered(job, r);\n",
     ""),
    ("a call's own number not kept", "qucscontrol.cpp",
     "a_running, Running{caller, tool, job});",
     "a_running, Running{caller, tool, 0});"),
    ("a queued call not said to wait", "qucscontrol.cpp",
     "        jobQueued(job);\n",
     ""),
    ("a batch's calls not counted", "qucscontrol.cpp",
     "        a_control->jobStep(a_call, index, int(a_calls.size()));\n",
     ""),
    ("a followed simulation's end not told", "qucscontrol_watch.cpp",
     "    jobSimEnded(id, run->result);\n",
     ""),
    ("a solve past its timeout over at its answer", "qucscontrol_jobs.cpp",
     "                it->followsDocument = true;\n",
     "                endJob(number, result);\n                return;\n"),
    ("a simulation's percentage not read", "qucscontrol_jobs.cpp",
     "percent = console->percent() > 0 ? console->percent() : -1;",
     "percent = -1;"),
    ("a model's percentage not read", "qucscontrol_jobs.cpp",
     "            percent = percentOfFraction(model->jobFraction());\n",
     ""),
    ("stops every call of its name", "qucscontrol_jobs.cpp",
     "        if (s.call.job == number) {\n            s.stop();",
     "        if (s.call.tool == it->tool) {\n            s.stop();"),
    ("failed called finished", "qucscontrol_jobs.cpp",
     "        } else if (failed) {\n            job.status = jobs::Status::Failed;\n        } else {",
     "        } else {"),
    ("a stop asked for not called stopped", "qucscontrol_jobs.cpp",
     "if ((job.stopAsked && failed) || o.value(QLatin1String(\"stopped\")).toBool()) {",
     "if (false) {"),
    ("every quick call a job", "qucscontrol_jobs.cpp",
     "    return !kindOfTool(tool).isEmpty();",
     "    return true;"),
    ("a wait a job", "qucscontrol_jobs.cpp",
     'it->tool != QLatin1String("wait_for") && it->age.elapsed() >= kListedPast',
     "it->age.elapsed() >= kListedPast"),
    ("the window opened under --mcp-server", "qucscontrol_jobs.cpp",
     "    if (a_headless) {\n        o.insert(QStringLiteral(\"shown\"), false);",
     "    if (false) {\n        o.insert(QStringLiteral(\"shown\"), false);"),
]


def run(build, wanted):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QUCS_CLAUDE="/nonexistent/claude", QUCS_GH="/nonexistent/gh")
    survived = []
    for name, file, old, new in BREAKS:
        if wanted and name not in wanted:
            continue
        path = os.path.join(SOURCE, file)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        if old not in text:
            print(f"NOT APPLIED  {name}: its text is not in {file}")
            survived.append(name)
            continue
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(old, new, 1))
            built = subprocess.run(["cmake", "--build", build, "--target", "test_claude_jobs", "test_mcp_server", "qucs-s", "--parallel", "12"],
                                   capture_output=True, text=True, stdin=subprocess.DEVNULL)
            if built.returncode != 0:
                print(f"NOT BUILT    {name}: {built.stdout[-400:]}")
                survived.append(name)
                continue
            failed = []
            for test, slots in (("test_claude_jobs", []), ("test_mcp_server", ["theLongToolsAreJobs", "theProgramServesOverStdio"])):
                try:
                    done = subprocess.run([os.path.join(build, "qucs", "tests", test)] + slots, capture_output=True, text=True, env=env,
                                          stdin=subprocess.DEVNULL, timeout=420)
                    lines = [l for l in done.stdout.splitlines() if l.startswith("FAIL!")]
                    if done.returncode != 0:
                        failed += [l.split("::", 1)[1].split("(", 1)[0] for l in lines] or [f"{test} ended with {done.returncode}"]
                except subprocess.TimeoutExpired:
                    failed.append(f"{test} did not end")
            if failed:
                print(f"caught       {name}: {', '.join(failed)}", flush=True)
            else:
                print(f"SURVIVED     {name}", flush=True)
                survived.append(name)
        finally:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
    print(f"{len(survived)} of those run not caught")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(run(os.path.abspath(sys.argv[1]), set(sys.argv[2:])))
