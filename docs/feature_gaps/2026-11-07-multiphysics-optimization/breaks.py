#!/usr/bin/env python3
# Breaks each part of the multiphysics optimization in turn, in a copy of the
# source, and runs the test that should catch it: a test that still passes is
# a test that proves nothing. Each break is put back before the next.
#
#   python3 breaks.py <source copy> <its build tree> [results.txt [words of the names to run]]
#
# Not on the checkout itself: other work may be building it. The copy is made
# with rsync, its build tree configured as Release; each break rebuilds what it
# touches and links the one test.
import os
import subprocess
import sys

SRC, BUILD = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
OUT = sys.argv[3] if len(sys.argv) > 3 else 'results.txt'
ONLY = sys.argv[4:]   # (only the breaks whose names hold one of these)

CORE, FEM, MODEL, DOC, PANEL, PAGE, MCP = ('qucs/multiphysics/optimizer_core.cpp', 'qucs/multiphysics/fem_optimize.cpp', 'qucs/multiphysics/fem_model.cpp',
                                            'qucs/multiphysicsdoc.cpp', 'qucs/multiphysicspanel.cpp', 'qucs/multiphysicsoptimization.cpp', 'qucs/qucscontrol_fem.cpp')
UNIT, ENGINE, WINDOW = 'test_optimizer_core', 'test_fem_optimize', 'test_multiphysics_optimize'

BREAKS = [
    # ---- The searches: against ngspice's numbers, known minima.
    ('Nelder-Mead expands by less', CORE, 'const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigma = 0.5;', 'const double alpha = 1.0, gamma = 1.2, rho = 0.5, sigma = 0.5;', UNIT),
    ('Nelder-Mead takes the worst vertex for the best', CORE, 'if (fr < fv[std::size_t(lo)]) {   // expand', 'if (fr > fv[std::size_t(lo)]) {   // expand', UNIT),
    ('Levenberg-Marquardt never lowers lambda', CORE, 'lambda *= 0.3;\n                if (lambda < 1e-12)', 'lambda *= 1.0;\n                if (lambda < 1e-12)', UNIT),
    ('a backward difference with the wrong sign', CORE, '(rs[std::size_t(j)][i] - r0[i]) / (sign[std::size_t(j)] * h)', '(rs[std::size_t(j)][i] - r0[i]) / h', UNIT),
    ('a swarm without its own best', CORE, 'phi * r1 * (pb[std::size_t(i)][std::size_t(j)] - xij)', 'phi * r1 * 0.0', UNIT),
    ('annealing without its moves per level', CORE, 'const int L = 8 + 4 * n;', 'const int L = 8;', UNIT),
    ('annealing never climbs', CORE, 'if (d <= 0.0 || rand() < std::exp(-d / T)) {', 'if (d <= 0.0) {', UNIT),
    ('the trust region never grows', CORE, 'delta = 2.0 * delta < 0.5 ? 2.0 * delta : 0.5;', 'delta = delta;', UNIT),
    ('the trust region\'s model ignores its curvature', CORE, 'H[std::size_t(i * n + l)] += lam * yh[std::size_t(j)][std::size_t(i)] * yh[std::size_t(j)][std::size_t(l)];', '(void) lam;', UNIT),
    ('CMA-ES without its step size', CORE, 'sigma *= std::exp((cs / damps) * (psn / chiN - 1.0));', '(void) damps;', UNIT),
    ('CMA-ES recombines the worst', CORE, 'R[std::size_t(i)] = F[std::size_t(i)] >= kPenalty ? kPenalty + P[std::size_t(i)] : F[std::size_t(i)] + scale * P[std::size_t(i)];', 'R[std::size_t(i)] = -F[std::size_t(i)];', UNIT),
    ('the surrogate\'s improvement of another sign', CORE, 'return (ybest - 0.01 - m) * cdf + s * pdf;', 'return (m - ybest) * cdf + s * pdf;', UNIT),
    ('Bayes hands nothing to the local method', CORE, 'm_polish = true;\n        m_polishIters = budget - N;', 'm_polishIters = 0;', UNIT),
    ('NSGA-II without crowding', CORE, 'crowd[std::size_t(idx[0])] = crowd[std::size_t(idx[std::size_t(nf - 1)])] = 1e30;', 'crowd[std::size_t(idx[0])] = crowd[std::size_t(idx[std::size_t(nf - 1)])] = 0;', UNIT),
    ('NSGA-II without mutation', CORE, 'if (rand() < pm) {   // polynomial mutation\n                    const double u = rand();\n                    ch1 +=', 'if (rand() < pm) {   // polynomial mutation\n                    const double u = rand();\n                    ch1 += 0 *', UNIT),
    ('NSGA-II\'s front of the lower ranks too', CORE, 'if (rank[std::size_t(i)] == 0 && V[std::size_t(i)] < kPenalty) fr.push_back(i);', 'if (V[std::size_t(i)] < kPenalty) fr.push_back(i);', UNIT),
    ('NSGA-II ignores limits', CORE, 'if (violA > 0 || violB > 0) {', 'if (false) {', UNIT),
    ('differential evolution without its stagnation', CORE, 'if (++stall >= 8 + n / 4) {\n                    m_status = SearchStatus::Converged;\n                    break;\n                }\n            } else {\n                stall = 0;\n            }\n        }\n    }\n    ubest = gb;\n    fbest = gf;\n}\n\n// -------------------------------------------------------- Simulated annealing',
     'if (false) {\n                    m_status = SearchStatus::Converged;\n                    break;\n                }\n            } else {\n                stall = 0;\n            }\n        }\n    }\n    ubest = gb;\n    fbest = gf;\n}\n\n// -------------------------------------------------------- Simulated annealing', UNIT),
    ('the DE variants all one', CORE, 'settings.method = std::clamp(m_options.strategy, 1, 10);', 'settings.method = 7;', UNIT),
    ('the multipliers not updated', CORE, 'k.lamLo = k.lamLo + m_rho * g > 0.0 ? k.lamLo + m_rho * g : 0.0;', 'k.lamLo = 0.0;', UNIT),
    ('the first penalty not balanced', CORE, 'm_rho = fs > 0.0 ? 2.0 * fs / (viol0 * viol0) : 1.0;', 'm_rho = 1.0;', UNIT),
    ('a limit counted by its raw miss', CORE, 'return upper ? (value - k.def.upper) / s : (k.def.lower - value) / s;', 'return upper ? (value - k.def.upper) : (k.def.lower - value);', ENGINE),
    ('the best not feasible first', CORE, 'const bool better = !m_hasBest || (feasible && !m_bestFeasible)\n                                || (feasible == m_bestFeasible && (feasible ? e.objective < m_bestObjective : viol < m_bestViolation));', 'const bool better = !m_hasBest || e.objective < m_bestObjective;', ENGINE),
    ('a stop not heard', CORE, 'if (m_interrupted || m_ev.stopRequested()) {\n        m_interrupted = true;', 'if (m_interrupted) {\n        m_interrupted = true;', UNIT),
    ('more starts with all the iterations', CORE, 'm_maxIter = std::max((full + nrun - 1) / nrun, 1);', 'm_maxIter = full;', UNIT),
    ('the polish by another method', CORE, 'const bool lm = m_problem.residuals > 0;', 'const bool lm = false;', UNIT),
    ('a failed start ends a local method', CORE, 'if (*std::min_element(fv.begin(), fv.end()) >= kPenalty && !halted()) {', 'if (false) {', UNIT),
    ('a failure counted as a success', CORE, 'const bool good = e.solved && std::isfinite(e.objective);', 'const bool good = e.solved;', UNIT),
    ('false position without Illinois', CORE, 'wa = 1;\n                wb *= 0.5;', 'wa = 1;', UNIT),
    ('Broyden without its update', CORE, 'J[std::size_t(j)][std::size_t(i)] += ((rn[std::size_t(j)] - r0[std::size_t(j)]) - jdu[std::size_t(j)]) * du[std::size_t(i)] / moved;', '(void) jdu;', UNIT),
    ('an end that fails not searched past', CORE, 'for (int side = 0; side < 2 && !bracketed && !known.empty(); ++side) {', 'for (int side = 0; side < 0; ++side) {', UNIT),
    ('an objective that never moved called converged', CORE, 'else if (m_seenN > 1 && m_seenHi == m_seenLo && !m_reached) m_status = SearchStatus::Unchanged;', '', UNIT),
    # ---- A model's optimization: closed forms.
    ('a log range read as a line', FEM, 'values.push_back(v.value(v.lower() + std::clamp(u, 0.0, 1.0) * (v.upper() - v.lower())));', 'values.push_back(v.value(v.min + std::clamp(u, 0.0, 1.0) * (v.max - v.min)));', ENGINE),
    ('the candidate\'s values not in the model', FEM, 'if (it != overrides.constEnd()) p.expression = *it;', '(void) it;', ENGINE),
    ('a sweep\'s points read without the candidate', FEM, 'StudyRun run = runStudy(candidate, ps,', 'StudyRun run = runStudy(a_model, ps,', ENGINE),
    ('every candidate on the first mesh', FEM, 'if (!a_shaping.reshapes(changed)) {', 'if (true) {', ENGINE),
    ('a row\'s worst as its last', FEM, 'const std::vector<Instance> at = instances(set, !o.last, 0,', 'const std::vector<Instance> at = instances(set, false, 0,', ENGINE),
    ('Over last read as worst', FEM, 'const std::vector<Instance> at = instances(set, !o.last, 0,', 'const std::vector<Instance> at = instances(set, true, 0,', ENGINE),
    ('a row\'s study not read', FEM, 'const int stage = o.study.isEmpty() ? 0 : int(a_plan.stages.indexOf(o.study));', 'const int stage = 0;', ENGINE),
    ('a target\'s miss not relative', FEM, 'const double r = o.weight * (y - a_values[i]) / s;', 'const double r = o.weight * (y - a_values[i]);', ENGINE),
    ('weights ignored', FEM, 'cost += (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * o.weight * y / s;', 'cost += (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * y / s;', ENGINE),
    ('MAX as MIN', FEM, 'cost -= (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * o.weight * y / s;', 'cost += (a_plan.penalty ? a_plan.objectiveWeight : 1.0) * o.weight * y / s;', ENGINE),
    ('a penalty that costs nothing', FEM, 'penalties += a_plan.constraintWeight * o.weight * miss / t;', 'penalties += 0 * miss / t;', ENGINE),
    ('a limits-only run does not stop', FEM, 'e.goalsMet = !anyGoal && allMet;', 'e.goalsMet = false;', ENGINE),
    ('Cpk from the wrong side', FEM, 'cpk = o.kind == OptimizationObjective::LessEqual ? (*limit - mean) / (3 * sigma) : (mean - *limit) / (3 * sigma);', 'cpk = (*limit - mean) / (3 * sigma);', ENGINE),
    ('a yield that counts nothing', FEM, 'if (pass) ++passed;', '', ENGINE),
    ('a spread not applied', FEM, 'overrides.insert(var.parameter, valueText(base + spread * a_draws[std::size_t(s)][j], a_params->dims.value(var.parameter)));', '(void) spread;', ENGINE),
    ('solved again each time', FEM, 'if (a_cache.find(keys[i]) == a_cache.end() && std::find(queued.begin(), queued.end(), keys[i]) == queued.end()) {', 'if (true) {', ENGINE),
    ('solves at once given back in another order', FEM, 'pool.start([this, k, from, values, &found] { found[k - from] = solveAt(values); });', 'pool.start([this, k, from, to, values, &found] { found[to - 1 - k] = solveAt(values); });', ENGINE),
    ('what the results lack not fatal', FEM, 'if (m.fatal) {\n                a_fatal = m.why;\n                continue;\n            }', '', ENGINE),
    ('why it did not solve not said', FEM, 'row.why = m.why;', '', ENGINE),
    ('the maximum may be below the minimum', FEM, 'if (!(*hi > *lo)) return refuse(tr("%1: %2: the maximum must be above the minimum").arg(plan.name, name));', '', ENGINE),
    ('a bare number as a length', FEM, 'if (e.dim() != dim) {', 'if (false) {', ENGINE),
    ('an unknown method runs', FEM, 'if (!method.isEmpty() && !optimization::searchMethod(method)) {', 'if (false) {', ENGINE),
    ('lm without targets runs', FEM, 'if (targets == 0 || minmax > 0) return refuse(tr("%1: Levenberg-Marquardt fits targets; it does not minimize an expression (MIN, MAX)").arg(plan.name));', '', ENGINE),
    ('the unit of what is written in', FEM, 'return QStringLiteral("%1[%2]").arg(QString::number((value - u->offset) / u->scale, \'g\', 12), m.captured(1));', '(void) u;', ENGINE),
    ('a front written in without a design', FEM, 'if (point < 1 || point > int(r.front.size())) {', 'if (false) {', ENGINE),
    ('what could not be met written in', FEM, 'if (status == QLatin1String("infeasible")) {', 'if (false) {', ENGINE),
    ('the file\'s rows without those that improved', FEM, 'if (!r.best && skip > 0) {', 'if (skip > 0) {', ENGINE),
    ('a row of too few cells read', FEM, 'if (cells.size() != 1 + values + measured + 3) return false;', 'if (cells.size() < 1) return false;', ENGINE),
    ('the setup of the varied parameters too', FEM, 'if (!varied.contains(p.toObject().value(QStringLiteral("name")).toString().trimmed())) parameters.append(p);', 'parameters.append(p);', ENGINE),
    ('the setup with its result', FEM, 'mine.remove(QStringLiteral("result"));', '', ENGINE),
    # ---- The file.
    ('the section not written', MODEL, 'if (!optimizations.isEmpty()) o.insert(QStringLiteral("optimizations"), optimizations);', '', ENGINE),
    ('the section written where there is none', MODEL, 'if (!optimizations.isEmpty()) o.insert(QStringLiteral("optimizations"), optimizations);', 'o.insert(QStringLiteral("optimizations"), optimizations);', ENGINE),
    ('the section not read', MODEL, 'for (const QJsonValue& s : o.value(QStringLiteral("optimizations")).toArray()) {', 'for (const QJsonValue& s : QJsonArray()) {', ENGINE),
    # ---- The window.
    ('what it found not kept', DOC, 'editNode(opt, [&result](Node& n) { n.props.insert(QStringLiteral("result"), result.toJson()); }, tr("Optimization Result"));', '(void) result;', WINDOW),
    ('a run\'s rows not shown as it goes', DOC, 'for (const OptimizationRow& row : rows) a_optimizationPage->addRow(row, columns);', '', WINDOW),
    ('a Cancel not heard', DOC, 'job.optimization = runOptimization(job.model, opt, job.parameters, built ? job.topology : nullptr, built ? job.mesh : nullptr, job.progress(0, 1), listener);', 'job.optimization = runOptimization(job.model, opt, job.parameters, built ? job.topology : nullptr, built ? job.mesh : nullptr, {}, listener);', WINDOW),
    ('a bad setup started', DOC, 'if (!optimizationPlan(a_model, *found, *parameters(), &why)) {', 'if (false) {', WINDOW),
    ('Apply not a step of Undo', DOC, 'setModel(m, tr("Apply Optimum of %1").arg(node->name()));', 'applyModel(m);', WINDOW),
    ('Apply does not compute', DOC, 'if (node) compute(node->text(QStringLiteral("study")));', '', WINDOW),
    ('the page not told of a change', DOC, '    updateOptimizationPage();\n    emit modelChanged();', '    emit modelChanged();', WINDOW),
    ('the button always there', DOC, 'a_optimize->setVisible(!optimizationNodes(a_model).empty());', 'a_optimize->setVisible(true);', WINDOW),
    ('a box\'s value not collected', PANEL, 'if (auto* box = qobject_cast<QComboBox*>(table->cellWidget(r, c))) return box->currentText().trimmed();', '', WINDOW),
    ('no boxes in a table', PANEL, 'if (!columnChoices.at(c).isEmpty()) {', 'if (false) {', WINDOW),
    ('a row without its range', FEM, 'return QJsonArray{name, QString(), parameterText(model, parameters, name, low), parameterText(model, parameters, name, high), QStringLiteral("LIN_DOUBLE"), QStringLiteral("yes")};', 'return QJsonArray{name, QString(), parameterText(model, parameters, name, high), parameterText(model, parameters, name, low), QStringLiteral("LIN_DOUBLE"), QStringLiteral("yes")};', WINDOW),
    ('Apply enabled for what cannot be', PAGE, 'a_apply->setEnabled(result.canApply());', 'a_apply->setEnabled(true);', WINDOW),
    ('a front chosen from nothing', PAGE, 'return items.isEmpty() ? 0 : items.first()->row() + 1;', 'return 0;', WINDOW),
    # ---- The tools.
    ('fem_describe gives the history', MCP, 'props.remove(QStringLiteral("result"));\n            o.insert(QStringLiteral("last result")', 'o.insert(QStringLiteral("last result")', WINDOW),
    ('a whole model\'s section refused', MCP, 'QStringLiteral("studies"),    QStringLiteral("optimizations"), QStringLiteral("results")};', 'QStringLiteral("studies"),    QStringLiteral("results")};', WINDOW),
    ('apply_optimum not read', MCP, 'const bool applyAfter = args.value(QLatin1String("apply_optimum")).toBool();', 'const bool applyAfter = false;', WINDOW),
    ('a run past its time said as done', MCP, 'return done(errorResult(tr("Still optimizing after %1 s, %2: it goes on (fem_describe says how far; fem_stop stops it with the best point so far; fem_optimize action=result reads it once it has ended).")\n                                            .arg(timeout)\n                                            .arg(d->jobProgress())));', 'return done(femOptimized(d, args, found));', WINDOW),
    ('a 3D model let through', MCP, 'return given(errorResult(error.isEmpty() ? tr("fem_optimize optimizes 2D models alone yet.") : error));', '(void) 0;', WINDOW),
]


def run(cmd, timeout=None):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=timeout, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'))


def results(text):
    return [l for l in text.splitlines() if l.startswith('FAIL!') or l.startswith('Totals')]


out = open(OUT, 'a')
caught = missed = 0
for what, file, old, new, test in BREAKS:
    if ONLY and not any(word in what for word in ONLY):
        continue
    path = os.path.join(SRC, file)
    original = open(path, encoding='utf-8').read()
    if original.count(old) < 1:
        print(f'NOT FOUND  {what}: {file}', file=out, flush=True)
        print(f'NOT FOUND  {what}', flush=True)
        missed += 1
        continue
    try:
        open(path, 'w', encoding='utf-8').write(original.replace(old, new, 1))
        built = run(['ninja', '-C', BUILD, test])
        if built.returncode != 0:
            print(f'DOES NOT BUILD  {what}: {built.stdout[-400:]}', file=out, flush=True)
            print(f'DOES NOT BUILD  {what}', flush=True)
            missed += 1
            continue
        binary = os.path.join(BUILD, 'qucs', 'tests', test)
        try:
            r = run([binary], timeout=400)
            fails = results(r.stdout)
            if r.returncode == 0:
                missed += 1
                print(f'MISSED  {what} ({test})', file=out, flush=True)
                print(f'MISSED  {what} ({test})', flush=True)
            else:
                caught += 1
                first = next((l for l in fails if l.startswith('FAIL!')), f'exit {r.returncode}')
                print(f'caught  {what} ({test}): {first[:150]}', file=out, flush=True)
                print(f'caught  {what}', flush=True)
        except subprocess.TimeoutExpired:
            caught += 1
            print(f'caught  {what} ({test}): past 400 s', file=out, flush=True)
            print(f'caught  {what}: past 400 s', flush=True)
    finally:
        open(path, 'w', encoding='utf-8').write(original)
print(f'{caught} caught, {missed} not, of {len(BREAKS)}', file=out, flush=True)
print(f'{caught} caught, {missed} not, of {len(BREAKS)}', flush=True)
