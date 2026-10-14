/*
 * fem_lumerical_script.cpp - a Lumerical script (.lsf) read as far as a
 *                            model is concerned
 *
 * This file is part of Qucs-S.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The language as its scripts use it: statements ending in ';', blocks in
 * braces, if/else, for (i = a:b) and C's for, try { } catch(e);, '#'
 * comments; numbers, texts ("..." or '...'), matrices [a, b; c, d] taken
 * element by element (+ - * / ^ elementwise, as Lumerical's are), ranges
 * a:b and a:s:b, structs (s.f), cells (c{i}), indexing from 1. The
 * commands that build a project (addrect, addpoly, addfde, set, setnamed,
 * select, addmaterial, setmaterial, setanalysis, addmodelmaterial,
 * addmaterialproperties, addsemiconductorfromalloy, addchargesolver,
 * addsimulationregion, adddope, adddiffusion, addimportdope,
 * rectilineardataset, importdataset, addelectricalcontact, mqwindex...)
 * record what they make; the others (save, run, findmodes, plot...) are
 * passed over, and a command not known said.
 */
#include "fem_lumerical_scene.h"

#include <QCoreApplication>

#include <cmath>
#include <functional>
#include <optional>

namespace qucs_s::fem::lumerical {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("LumericalScript", text);
}

// --- Tokens -------------------------------------------------------------

struct Token {
    enum Kind { Number, Text, Name, Op, End };
    Kind kind = End;
    QString s;
    double v = 0;
    int line = 1;
};

std::vector<Token> tokenize(const QString& text, QString* error)
{
    std::vector<Token> out;
    int line = 1;
    const int n = int(text.size());
    for (int i = 0; i < n;) {
        const QChar c = text[i];
        if (c == QLatin1Char('\n')) {
            ++line;
            ++i;
            continue;
        }
        if (c.isSpace()) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('#')) {
            while (i < n && text[i] != QLatin1Char('\n')) ++i;
            continue;
        }
        Token t;
        t.line = line;
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            // (A quote within: doubled, or a text of the other quote.)
            int j = i + 1;
            QString s;
            for (; j < n; ++j) {
                if (text[j] == c) {
                    if (j + 1 < n && text[j + 1] == c) {
                        s += c;
                        ++j;
                        continue;
                    }
                    break;
                }
                if (text[j] == QLatin1Char('\n')) ++line;
                s += text[j];
            }
            if (j >= n) {
                *error = tr("line %1: a text not ended").arg(t.line);
                return {};
            }
            t.kind = Token::Text;
            t.s = s;
            out.push_back(t);
            i = j + 1;
            continue;
        }
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && text[i + 1].isDigit())) {
            int j = i;
            while (j < n && (text[j].isDigit() || text[j] == QLatin1Char('.'))) ++j;
            if (j < n && (text[j] == QLatin1Char('e') || text[j] == QLatin1Char('E'))) {
                int k = j + 1;
                if (k < n && (text[k] == QLatin1Char('+') || text[k] == QLatin1Char('-'))) ++k;
                if (k < n && text[k].isDigit()) {
                    j = k;
                    while (j < n && text[j].isDigit()) ++j;
                }
            }
            bool ok = false;
            t.kind = Token::Number;
            t.v = text.mid(i, j - i).toDouble(&ok);
            if (!ok) {
                *error = tr("line %1: %2 is not a number").arg(line).arg(text.mid(i, j - i));
                return {};
            }
            out.push_back(t);
            i = j;
            continue;
        }
        if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < n && (text[j].isLetterOrNumber() || text[j] == QLatin1Char('_'))) ++j;
            t.kind = Token::Name;
            t.s = text.mid(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        static const QStringList two = {QStringLiteral("=="), QStringLiteral("!="), QStringLiteral("~="), QStringLiteral("<="),
                                        QStringLiteral(">="), QStringLiteral("&&"), QStringLiteral("||")};
        const QString pair = text.mid(i, 2);
        t.kind = Token::Op;
        if (two.contains(pair)) {
            t.s = pair;
            i += 2;
        } else {
            t.s = QString(c);
            i += 1;
        }
        out.push_back(t);
    }
    Token end;
    end.line = line;
    out.push_back(end);
    return out;
}

// --- The syntax ---------------------------------------------------------

struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Num, Str, Name, Call, Cell, Field, Unary, Binary, Matrix, Range };
    Kind kind = Num;
    double v = 0;
    QString s;                          // a name, an operator, a field
    std::vector<ExprP> args;            // a call's, an index's; a unary's, a binary's, a range's
    std::vector<std::vector<ExprP>> rows;   // a matrix's
    int line = 1;
};

struct Stmt;
using StmtP = std::shared_ptr<Stmt>;
struct Stmt {
    enum Kind { Eval, Assign, If, For, CFor, While, Try, Block, Nop };
    Kind kind = Nop;
    ExprP target, value, cond;
    StmtP init, step;                   // C's for
    std::vector<StmtP> body, other;     // (if: its else)
    QString var;                        // for's, catch's
    int line = 1;
};

class Parser
{
public:
    Parser(std::vector<Token> t) : a_t(std::move(t)) {}
    QString error;

    std::vector<StmtP> program()
    {
        std::vector<StmtP> out;
        while (!at(Token::End) && error.isEmpty()) {
            StmtP s = statement();
            if (s) out.push_back(s);
        }
        return out;
    }

private:
    std::vector<Token> a_t;
    std::size_t a_p = 0;

    const Token& peek(int k = 0) const { return a_t[std::min(a_t.size() - 1, a_p + std::size_t(k))]; }
    bool at(Token::Kind k) const { return peek().kind == k; }
    bool isOp(const char* op, int k = 0) const { return peek(k).kind == Token::Op && peek(k).s == QLatin1String(op); }
    bool isName(const char* name) const { return peek().kind == Token::Name && peek().s == QLatin1String(name); }
    void fail(const QString& why)
    {
        if (error.isEmpty()) error = tr("line %1: %2").arg(peek().line).arg(why);
        a_p = a_t.size() - 1;
    }
    bool expect(const char* op)
    {
        if (isOp(op)) {
            ++a_p;
            return true;
        }
        fail(tr("'%1' wanted, %2 found").arg(QLatin1String(op), peek().kind == Token::End ? tr("the end") : QStringLiteral("'%1'").arg(peek().s.isEmpty() ? QString::number(peek().v) : peek().s)));
        return false;
    }
    void semicolons()
    {
        while (isOp(";")) ++a_p;
    }

    std::vector<StmtP> block()
    {
        std::vector<StmtP> out;
        if (isOp("{")) {
            ++a_p;
            while (!isOp("}") && !at(Token::End) && error.isEmpty()) {
                StmtP s = statement();
                if (s) out.push_back(s);
            }
            expect("}");
        } else {
            StmtP s = statement();
            if (s) out.push_back(s);
        }
        return out;
    }

    StmtP statement()
    {
        auto s = std::make_shared<Stmt>();
        s->line = peek().line;
        if (isOp(";")) {
            ++a_p;
            return nullptr;
        }
        if (isOp("{")) {
            s->kind = Stmt::Block;
            s->body = block();
            return s;
        }
        if (isOp("?")) {
            // (A print: its value worked out, passed over.)
            ++a_p;
            s->kind = Stmt::Eval;
            s->value = expression();
            semicolons();
            return s;
        }
        if (isName("if") && isOp("(", 1)) {
            a_p += 2;
            s->kind = Stmt::If;
            s->cond = expression();
            expect(")");
            s->body = block();
            if (isName("else")) {
                ++a_p;
                s->other = block();
            }
            return s;
        }
        if (isName("for") && isOp("(", 1)) {
            a_p += 2;
            // for (i = a:b) or for (init; cond; step).
            const std::size_t back = a_p;
            int depth = 0;
            bool semicolon = false;
            for (std::size_t k = a_p; k < a_t.size(); ++k) {
                if (a_t[k].kind == Token::Op && a_t[k].s == QLatin1String("(")) ++depth;
                if (a_t[k].kind == Token::Op && a_t[k].s == QLatin1String(")")) {
                    if (depth == 0) break;
                    --depth;
                }
                if (depth == 0 && a_t[k].kind == Token::Op && a_t[k].s == QLatin1String(";")) {
                    semicolon = true;
                    break;
                }
            }
            a_p = back;
            if (semicolon) {
                s->kind = Stmt::CFor;
                s->init = simple();
                expect(";");
                s->cond = expression();
                expect(";");
                s->step = simple();
                expect(")");
            } else {
                s->kind = Stmt::For;
                if (!at(Token::Name)) {
                    fail(tr("a loop's variable wanted"));
                    return nullptr;
                }
                s->var = peek().s;
                ++a_p;
                expect("=");
                s->value = expression();
                expect(")");
            }
            s->body = block();
            return s;
        }
        if (isName("while") && isOp("(", 1)) {
            a_p += 2;
            s->kind = Stmt::While;
            s->cond = expression();
            expect(")");
            s->body = block();
            return s;
        }
        if (isName("try") && isOp("{", 1)) {
            ++a_p;
            s->kind = Stmt::Try;
            s->body = block();
            if (isName("catch")) {
                ++a_p;
                if (expect("(")) {
                    if (at(Token::Name)) {
                        s->var = peek().s;
                        ++a_p;
                    }
                    expect(")");
                }
            }
            semicolons();
            return s;
        }
        if (isName("function")) {
            fail(tr("functions of its own are not read"));
            return nullptr;
        }
        StmtP t = simple();
        if (t && !isOp("}")) {
            if (!at(Token::End) && !isOp(";")) {
                fail(tr("';' wanted"));
                return nullptr;
            }
            semicolons();
        }
        return t;
    }

    /// An assignment or an expression.
    StmtP simple()
    {
        auto s = std::make_shared<Stmt>();
        s->line = peek().line;
        ExprP e = expression();
        if (isOp("=")) {
            ++a_p;
            if (e->kind != Expr::Name && e->kind != Expr::Field && e->kind != Expr::Cell && e->kind != Expr::Call) {
                fail(tr("nothing to assign to"));
                return nullptr;
            }
            s->kind = Stmt::Assign;
            s->target = e;
            s->value = expression();
        } else {
            s->kind = Stmt::Eval;
            s->value = e;
        }
        return s;
    }

    ExprP make(Expr::Kind k)
    {
        auto e = std::make_shared<Expr>();
        e->kind = k;
        e->line = peek().line;
        return e;
    }
    ExprP binary(const QString& op, ExprP a, ExprP b)
    {
        ExprP e = make(Expr::Binary);
        e->s = op;
        e->args = {a, b};
        return e;
    }

    ExprP expression() { return range(); }
    ExprP range()
    {
        ExprP a = logical();
        if (isOp(":")) {
            ExprP e = make(Expr::Range);
            ++a_p;
            ExprP b = logical();
            if (isOp(":")) {
                ++a_p;
                ExprP c = logical();
                e->args = {a, b, c};
            } else
                e->args = {a, nullptr, b};
            return e;
        }
        return a;
    }
    ExprP logical()
    {
        ExprP a = comparison();
        while (isOp("&") || isOp("|") || isOp("&&") || isOp("||")) {
            const QString op = peek().s.left(1);
            ++a_p;
            a = binary(op, a, comparison());
        }
        return a;
    }
    ExprP comparison()
    {
        ExprP a = additive();
        while (isOp("==") || isOp("!=") || isOp("~=") || isOp("<") || isOp(">") || isOp("<=") || isOp(">=")) {
            QString op = peek().s;
            if (op == QLatin1String("~=")) op = QStringLiteral("!=");
            ++a_p;
            a = binary(op, a, additive());
        }
        return a;
    }
    ExprP additive()
    {
        ExprP a = multiplicative();
        while (isOp("+") || isOp("-")) {
            const QString op = peek().s;
            ++a_p;
            a = binary(op, a, multiplicative());
        }
        return a;
    }
    ExprP multiplicative()
    {
        ExprP a = unary();
        while (isOp("*") || isOp("/")) {
            const QString op = peek().s;
            ++a_p;
            a = binary(op, a, unary());
        }
        return a;
    }
    ExprP unary()
    {
        if (isOp("-") || isOp("+") || isOp("!") || isOp("~")) {
            ExprP e = make(Expr::Unary);
            e->s = peek().s == QLatin1String("~") ? QStringLiteral("!") : peek().s;
            ++a_p;
            e->args = {unary()};
            return e;
        }
        return power();
    }
    ExprP power()
    {
        ExprP a = postfix();
        if (isOp("^")) {
            ++a_p;
            return binary(QStringLiteral("^"), a, unary());
        }
        return a;
    }
    std::vector<ExprP> arguments(const char* close)
    {
        std::vector<ExprP> out;
        if (isOp(close)) {
            ++a_p;
            return out;
        }
        while (error.isEmpty()) {
            out.push_back(expression());
            if (isOp(",")) {
                ++a_p;
                continue;
            }
            expect(close);
            break;
        }
        return out;
    }
    ExprP postfix()
    {
        ExprP e = primary();
        while (error.isEmpty()) {
            if (isOp("(")) {
                ++a_p;
                ExprP c = make(Expr::Call);
                c->args = arguments(")");
                c->args.insert(c->args.begin(), e);
                e = c;
            } else if (isOp("{")) {
                ++a_p;
                ExprP c = make(Expr::Cell);
                c->args = arguments("}");
                c->args.insert(c->args.begin(), e);
                e = c;
            } else if (isOp(".") && peek(1).kind == Token::Name) {
                ExprP f = make(Expr::Field);
                f->s = peek(1).s;
                f->args = {e};
                a_p += 2;
                e = f;
            } else
                break;
        }
        return e;
    }
    ExprP primary()
    {
        const Token& t = peek();
        if (t.kind == Token::Number) {
            ExprP e = make(Expr::Num);
            e->v = t.v;
            ++a_p;
            return e;
        }
        if (t.kind == Token::Text) {
            ExprP e = make(Expr::Str);
            e->s = t.s;
            ++a_p;
            return e;
        }
        if (t.kind == Token::Name) {
            ExprP e = make(Expr::Name);
            e->s = t.s;
            ++a_p;
            return e;
        }
        if (isOp("(")) {
            ++a_p;
            ExprP e = expression();
            expect(")");
            return e;
        }
        if (isOp("[")) {
            // [a, b; c, d]: its rows (a struct's JSON-like {} is not read).
            ++a_p;
            ExprP e = make(Expr::Matrix);
            std::vector<ExprP> row;
            while (error.isEmpty()) {
                if (isOp("]")) {
                    ++a_p;
                    break;
                }
                row.push_back(expression());
                if (isOp(",")) ++a_p;
                else if (isOp(";")) {
                    ++a_p;
                    e->rows.push_back(row);
                    row.clear();
                } else if (!isOp("]")) {
                    fail(tr("',' ';' or ']' wanted in a matrix"));
                    break;
                }
            }
            if (!row.empty()) e->rows.push_back(row);
            return e;
        }
        fail(t.kind == Token::End ? tr("the script ends within a statement") : tr("'%1' not expected").arg(t.s));
        return make(Expr::Num);
    }
};

// --- Running it ---------------------------------------------------------

struct Thrown {
    QString why;
};

class Interpreter
{
public:
    Interpreter(Scene& scene) : a_scene(scene) {}

    void run(const std::vector<StmtP>& program)
    {
        try {
            for (const StmtP& s : program) exec(*s);
        } catch (const Thrown& t) {
            if (a_scene.error.isEmpty()) a_scene.error = t.why;
        } catch (const Stop&) {
        }
        for (const QString& n : a_unknown) a_scene.notes << tr("%1: not read (passed over)").arg(n);
    }

private:
    Scene& a_scene;
    std::map<QString, Value> a_vars;
    int a_current = -1;      // the object selected
    int a_fde = -1;          // the FDE (setanalysis's)
    int a_tries = 0;
    int a_steps = 0;
    QStringList a_unknown;
    struct Stop {};

    [[noreturn]] void raise(const QString& why, int line) { throw Thrown{tr("line %1: %2").arg(line).arg(why)}; }

    // ... values
    static Value scalar(double v) { return Value::number(v); }
    double number(const Value& v, int line)
    {
        if (v.kind != Value::Matrix || v.m.empty()) raise(tr("a number wanted"), line);
        return v.m[0];
    }
    bool truth(const Value& v)
    {
        if (v.kind == Value::Text) return !v.s.isEmpty();
        if (v.kind != Value::Matrix || v.m.empty()) return false;
        for (double x : v.m)
            if (x == 0 || std::isnan(x)) return false;
        return true;
    }

    Value elementwise(const QString& op, const Value& a, const Value& b, int line)
    {
        if (op == QLatin1String("+") && (a.kind == Value::Text || b.kind == Value::Text)) return Value::text(a.toText() + b.toText());
        if ((op == QLatin1String("==") || op == QLatin1String("!=")) && (a.kind == Value::Text || b.kind == Value::Text)) {
            const bool same = a.kind == b.kind && a.toText() == b.toText();
            return scalar(op == QLatin1String("==") ? same : !same);
        }
        if (a.kind != Value::Matrix || b.kind != Value::Matrix) raise(tr("'%1' of what is not a number").arg(op), line);
        if (a.m.empty() || b.m.empty()) return Value::matrix({}, 0, 0);
        const bool as = a.m.size() == 1, bs = b.m.size() == 1;
        if (!as && !bs && a.m.size() != b.m.size()) raise(tr("'%1' of matrices of other sizes").arg(op), line);
        const Value& shape = as ? b : a;
        Value out = Value::matrix(std::vector<double>(shape.m.size()), shape.rows, shape.cols);
        out.dims = shape.dims;
        for (std::size_t i = 0; i < out.m.size(); ++i) {
            const double x = a.m[as ? 0 : i], y = b.m[bs ? 0 : i];
            double r = 0;
            if (op == QLatin1String("+")) r = x + y;
            else if (op == QLatin1String("-")) r = x - y;
            else if (op == QLatin1String("*")) r = x * y;
            else if (op == QLatin1String("/")) r = x / y;
            else if (op == QLatin1String("^")) r = std::pow(x, y);
            else if (op == QLatin1String("==")) r = x == y;
            else if (op == QLatin1String("!=")) r = x != y;
            else if (op == QLatin1String("<")) r = x < y;
            else if (op == QLatin1String(">")) r = x > y;
            else if (op == QLatin1String("<=")) r = x <= y;
            else if (op == QLatin1String(">=")) r = x >= y;
            else if (op == QLatin1String("&")) r = (x != 0) && (y != 0);
            else if (op == QLatin1String("|")) r = (x != 0) || (y != 0);
            out.m[i] = r;
        }
        return out;
    }

    Value concatenate(const std::vector<std::vector<Value>>& rows, int line)
    {
        // Each row: its parts side by side (as many rows each); the rows
        // one below the other (as many columns each).
        std::vector<Value> stacked;
        for (const auto& row : rows) {
            int r = -1, cols = 0;
            std::vector<const Value*> parts;
            for (const Value& v : row) {
                if (v.kind == Value::Text && row.size() == 1 && rows.size() == 1) return v;
                if (v.kind != Value::Matrix) raise(tr("a matrix of what is not a number"), line);
                if (v.m.empty()) continue;
                if (r >= 0 && v.rows != r) raise(tr("a matrix's row of parts of other heights"), line);
                r = v.rows;
                cols += v.cols;
                parts.push_back(&v);
            }
            if (r < 0) continue;
            Value m = Value::matrix(std::vector<double>(std::size_t(r * cols)), r, cols);
            int c0 = 0;
            for (const Value* p : parts) {
                for (int j = 0; j < p->cols; ++j)
                    for (int i = 0; i < r; ++i) m.m[std::size_t((c0 + j) * r + i)] = p->m[std::size_t(j * r + i)];
                c0 += p->cols;
            }
            stacked.push_back(m);
        }
        if (stacked.empty()) return Value::matrix({}, 0, 0);
        int rowsTotal = 0;
        const int cols = stacked.front().cols;
        for (const Value& v : stacked) {
            if (v.cols != cols) raise(tr("a matrix's rows of other widths"), line);
            rowsTotal += v.rows;
        }
        Value m = Value::matrix(std::vector<double>(std::size_t(rowsTotal * cols)), rowsTotal, cols);
        int r0 = 0;
        for (const Value& v : stacked) {
            for (int j = 0; j < cols; ++j)
                for (int i = 0; i < v.rows; ++i) m.m[std::size_t(j * rowsTotal + r0 + i)] = v.m[std::size_t(j * v.rows + i)];
            r0 += v.rows;
        }
        return m;
    }

    Value rangeOf(double a, double s, double b, int line)
    {
        if (s == 0 || !std::isfinite(a + s + b)) raise(tr("a range's step must be other than 0"), line);
        std::vector<double> v;
        for (double x = a; s > 0 ? x <= b + 1e-12 * std::abs(s) : x >= b - 1e-12 * std::abs(s); x += s) {
            v.push_back(x);
            if (v.size() > 10000000) raise(tr("a range too long"), line);
        }
        const int n = int(v.size());
        return Value::matrix(std::move(v), 1, n);
    }

    // ... the objects
    Object* current(int line, const char* what)
    {
        if (a_current < 0 || a_current >= int(a_scene.objects.size())) raise(tr("%1: nothing is selected").arg(QLatin1String(what)), line);
        return &a_scene.objects[std::size_t(a_current)];
    }
    int named(QString name) const
    {
        // (A scope's prefix - "CHARGE::p well" - its own.)
        if (name.contains(QLatin1String("::"))) name = name.section(QStringLiteral("::"), -1);
        for (int i = int(a_scene.objects.size()) - 1; i >= 0; --i)
            if (a_scene.objects[std::size_t(i)].name() == name) return i;
        for (int i = int(a_scene.objects.size()) - 1; i >= 0; --i)
            if (a_scene.objects[std::size_t(i)].kind.compare(name, Qt::CaseInsensitive) == 0) return i;
        if (name == QLatin1String("FDE")) return a_fde;
        return -1;
    }
    void add(const QString& kind, const QString& name)
    {
        Object o;
        o.kind = kind;
        o.props[QStringLiteral("name")] = Value::text(name);
        a_scene.objects.push_back(o);
        a_current = int(a_scene.objects.size()) - 1;
        if (kind == QLatin1String("fde")) a_fde = a_current;
    }
    Material* material(const QString& name)
    {
        for (Material& m : a_scene.materials)
            if (m.name == name) return &m;
        return nullptr;
    }

    // ... evaluating
    Value eval(const Expr& e)
    {
        switch (e.kind) {
        case Expr::Num: return scalar(e.v);
        case Expr::Str: return Value::text(e.s);
        case Expr::Name: {
            const auto it = a_vars.find(e.s);
            if (it != a_vars.end()) return it->second;
            return call(e.s, {}, e.line);
        }
        case Expr::Unary: {
            const Value a = eval(*e.args[0]);
            if (e.s == QLatin1String("+")) return a;
            if (e.s == QLatin1String("!")) return scalar(!truth(a));
            return elementwise(QStringLiteral("-"), scalar(0), a, e.line);
        }
        case Expr::Binary: return elementwise(e.s, eval(*e.args[0]), eval(*e.args[1]), e.line);
        case Expr::Range: {
            const double a = number(eval(*e.args[0]), e.line), b = number(eval(*e.args[2]), e.line);
            const double s = e.args[1] ? number(eval(*e.args[1]), e.line) : 1.0;
            return rangeOf(a, s, b, e.line);
        }
        case Expr::Matrix: {
            std::vector<std::vector<Value>> rows;
            for (const auto& row : e.rows) {
                std::vector<Value> r;
                for (const ExprP& x : row) r.push_back(eval(*x));
                rows.push_back(r);
            }
            return concatenate(rows, e.line);
        }
        case Expr::Field: {
            const Value a = eval(*e.args[0]);
            if (const Value* f = a.field(e.s)) return *f;
            return Value();
        }
        case Expr::Cell: {
            const Value a = eval(*e.args[0]);
            if (a.kind != Value::Cell || e.args.size() < 2) raise(tr("{} of what is not a cell"), e.line);
            const int i = int(number(eval(*e.args[1]), e.line));
            if (i < 1 || i > int(a.cell.size())) raise(tr("a cell's index %1 out of 1 to %2").arg(i).arg(a.cell.size()), e.line);
            return a.cell[std::size_t(i - 1)];
        }
        case Expr::Call: {
            const Expr& f = *e.args[0];
            std::vector<ExprP> rest(e.args.begin() + 1, e.args.end());
            if (f.kind == Expr::Name) {
                const auto it = a_vars.find(f.s);
                if (it != a_vars.end()) return index(it->second, rest, e.line);
                std::vector<Value> args;
                for (const ExprP& x : rest) args.push_back(eval(*x));
                return call(f.s, args, e.line);
            }
            if (f.kind == Expr::Field) {
                // A dataset's method: d.addattribute("N", N).
                const Expr& owner = *f.args[0];
                if (owner.kind == Expr::Name) {
                    const auto it = a_vars.find(owner.s);
                    if (it != a_vars.end() && it->second.kind == Value::Dataset) {
                        std::vector<Value> args;
                        for (const ExprP& x : rest) args.push_back(eval(*x));
                        if ((f.s == QLatin1String("addattribute") || f.s == QLatin1String("addparameter")) && args.size() >= 2)
                            it->second.fields[(f.s == QLatin1String("addparameter") ? QStringLiteral("@") : QString()) + args[0].toText()] = args[1];
                        return Value();
                    }
                }
            }
            return index(eval(f), rest, e.line);
        }
        }
        return Value();
    }

    Value index(const Value& v, const std::vector<ExprP>& at, int line)
    {
        if (at.empty()) return v;
        if (v.kind != Value::Matrix) raise(tr("an index of what is not a matrix"), line);
        std::vector<std::vector<double>> picks;
        for (const ExprP& x : at) {
            const Value i = eval(*x);
            if (i.kind != Value::Matrix) raise(tr("an index must be numbers"), line);
            picks.push_back(i.m);
        }
        std::vector<double> out;
        if (picks.size() == 1) {
            for (double i : picks[0]) {
                const int k = int(std::lround(i));
                if (k < 1 || k > int(v.m.size())) raise(tr("index %1 out of 1 to %2").arg(k).arg(v.m.size()), line);
                out.push_back(v.m[std::size_t(k - 1)]);
            }
            const int n = int(out.size());
            return v.cols == 1 ? Value::matrix(out, n, 1) : Value::matrix(out, 1, n);
        }
        for (double j : picks[1])
            for (double i : picks[0]) {
                const int r = int(std::lround(i)), c = int(std::lround(j));
                if (r < 1 || r > v.rows || c < 1 || c > v.cols) raise(tr("index (%1, %2) out of %3 x %4").arg(r).arg(c).arg(v.rows).arg(v.cols), line);
                out.push_back(v.m[std::size_t((c - 1) * v.rows + r - 1)]);
            }
        return Value::matrix(out, int(picks[0].size()), int(picks[1].size()));
    }

    /// The place an assignment's target is (made as it is reached).
    Value& place(const Expr& e)
    {
        if (e.kind == Expr::Name) return a_vars[e.s];
        if (e.kind == Expr::Field) {
            Value& owner = place(*e.args[0]);
            if (owner.kind == Value::Null) owner.kind = Value::Struct;
            if (owner.kind != Value::Struct && owner.kind != Value::Dataset) raise(tr("a field of what is not a struct"), e.line);
            return owner.fields[e.s];
        }
        if (e.kind == Expr::Cell) {
            Value& owner = place(*e.args[0]);
            if (owner.kind == Value::Null) owner.kind = Value::Cell;
            if (owner.kind != Value::Cell || e.args.size() < 2) raise(tr("{} of what is not a cell"), e.line);
            const int i = int(number(eval(*e.args[1]), e.line));
            if (i < 1 || i > 100000) raise(tr("a cell's index %1").arg(i), e.line);
            if (int(owner.cell.size()) < i) owner.cell.resize(std::size_t(i));
            return owner.cell[std::size_t(i - 1)];
        }
        raise(tr("nothing to assign to"), e.line);
    }

    void assign(const Expr& target, const Value& v, int line)
    {
        if (target.kind == Expr::Call) {
            // a(i) = v: an element of a matrix.
            const Expr& owner = *target.args[0];
            if (owner.kind != Expr::Name || target.args.size() != 2) raise(tr("only a(i) = value is read"), line);
            Value& m = a_vars[owner.s];
            const int i = int(number(eval(*target.args[1]), line));
            if (m.kind == Value::Null) m = Value::matrix({}, 0, 0);
            if (m.kind != Value::Matrix || i < 1 || i > 10000000) raise(tr("a(%1) = value of what is not a matrix").arg(i), line);
            if (int(m.m.size()) < i) {
                m.m.resize(std::size_t(i), 0.0);
                if (m.cols <= 1 && m.rows > 1) m.rows = i;
                else {
                    m.rows = 1;
                    m.cols = i;
                }
            }
            m.m[std::size_t(i - 1)] = number(v, line);
            return;
        }
        place(target) = v;
    }

    void exec(const Stmt& s)
    {
        if (++a_steps > 20000000) raise(tr("the script runs too long"), s.line);
        switch (s.kind) {
        case Stmt::Nop: return;
        case Stmt::Eval: eval(*s.value); return;
        case Stmt::Assign: assign(*s.target, eval(*s.value), s.line); return;
        case Stmt::Block:
            for (const StmtP& x : s.body) exec(*x);
            return;
        case Stmt::If:
            if (truth(eval(*s.cond)))
                for (const StmtP& x : s.body) exec(*x);
            else
                for (const StmtP& x : s.other) exec(*x);
            return;
        case Stmt::For: {
            const Value range = eval(*s.value);
            if (range.kind != Value::Matrix) raise(tr("a loop over what is not numbers"), s.line);
            for (double x : range.m) {
                a_vars[s.var] = scalar(x);
                for (const StmtP& b : s.body) exec(*b);
            }
            return;
        }
        case Stmt::CFor:
            if (s.init) exec(*s.init);
            while (truth(eval(*s.cond))) {
                for (const StmtP& b : s.body) exec(*b);
                if (s.step) exec(*s.step);
            }
            return;
        case Stmt::While:
            while (truth(eval(*s.cond)))
                for (const StmtP& b : s.body) exec(*b);
            return;
        case Stmt::Try: {
            ++a_tries;
            const int objects = int(a_scene.objects.size());
            try {
                for (const StmtP& x : s.body) exec(*x);
            } catch (const Thrown& t) {
                // (What it added before it failed kept, as Lumerical keeps it.)
                (void)objects;
                if (!s.var.isEmpty()) a_vars[s.var] = Value::text(t.why);
            }
            --a_tries;
            return;
        }
        }
    }

    // ... the commands
    Value call(const QString& name, const std::vector<Value>& args, int line)
    {
        auto arg = [&](std::size_t i) -> const Value& {
            if (i >= args.size()) raise(tr("%1 wants more arguments").arg(name), line);
            return args[i];
        };
        auto num = [&](std::size_t i) { return number(arg(i), line); };
        // The constants.
        static const std::map<QString, double> constants = {{QStringLiteral("pi"), M_PI},
                                                            {QStringLiteral("c"), 299792458.0},
                                                            {QStringLiteral("eps0"), 8.8541878128e-12},
                                                            {QStringLiteral("mu0"), 1.25663706212e-6},
                                                            {QStringLiteral("e"), 1.602176634e-19},
                                                            {QStringLiteral("h"), 6.62607015e-34},
                                                            {QStringLiteral("hbar"), 1.054571817e-34},
                                                            {QStringLiteral("kb"), 1.380649e-23},
                                                            {QStringLiteral("true"), 1.0},
                                                            {QStringLiteral("false"), 0.0}};
        if (args.empty())
            if (const auto it = constants.find(name); it != constants.end()) return scalar(it->second);
        // The functions.
        static const std::map<QString, double (*)(double)> unary = {
            {QStringLiteral("sqrt"), [](double x) { return std::sqrt(x); }},  {QStringLiteral("abs"), [](double x) { return std::abs(x); }},
            {QStringLiteral("exp"), [](double x) { return std::exp(x); }},    {QStringLiteral("log"), [](double x) { return std::log(x); }},
            {QStringLiteral("log10"), [](double x) { return std::log10(x); }}, {QStringLiteral("sin"), [](double x) { return std::sin(x); }},
            {QStringLiteral("cos"), [](double x) { return std::cos(x); }},    {QStringLiteral("tan"), [](double x) { return std::tan(x); }},
            {QStringLiteral("round"), [](double x) { return std::round(x); }}, {QStringLiteral("floor"), [](double x) { return std::floor(x); }},
            {QStringLiteral("ceil"), [](double x) { return std::ceil(x); }},  {QStringLiteral("real"), [](double x) { return x; }},
            {QStringLiteral("imag"), [](double) { return 0.0; }}};
        if (const auto it = unary.find(name); it != unary.end()) {
            const Value& a = arg(0);
            if (a.kind != Value::Matrix) raise(tr("%1 of what is not a number").arg(name), line);
            Value out = a;
            for (double& x : out.m) x = it->second(x);
            return out;
        }
        if (name == QLatin1String("linspace")) {
            const double a = num(0), b = num(1);
            const int n = int(num(2));
            if (n < 1 || n > 10000000) raise(tr("linspace's count %1").arg(n), line);
            std::vector<double> v(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) v[std::size_t(i)] = n == 1 ? b : a + (b - a) * i / (n - 1);
            return Value::matrix(std::move(v), n, 1);
        }
        if (name == QLatin1String("reshape")) {
            Value out = arg(0);
            const Value& d = arg(1);
            std::size_t total = 1;
            out.dims.clear();
            for (double x : d.m) {
                out.dims.push_back(int(x));
                total *= std::size_t(std::max(0.0, x));
            }
            if (total != out.m.size()) raise(tr("reshape: %1 values are not %2").arg(out.m.size()).arg(total), line);
            if (!out.dims.empty()) {
                out.rows = out.dims[0];
                out.cols = int(out.m.size()) / std::max(1, out.rows);
            }
            return out;
        }
        if (name == QLatin1String("length")) {
            const Value& a = arg(0);
            return scalar(a.kind == Value::Cell ? double(a.cell.size()) : a.kind == Value::Text ? double(a.s.size()) : double(a.m.size()));
        }
        if (name == QLatin1String("size")) {
            const Value& a = arg(0);
            return Value::matrix({double(a.rows), double(a.cols)}, 1, 2);
        }
        if (name == QLatin1String("num2str")) {
            const Value& a = arg(0);
            return Value::text(a.kind == Value::Text ? a.s : QString::number(a.toNumber(), 'g', 12));
        }
        if (name == QLatin1String("min") || name == QLatin1String("max") || name == QLatin1String("sum")) {
            std::vector<double> all;
            for (const Value& a : args) all.insert(all.end(), a.m.begin(), a.m.end());
            if (all.empty()) return Value::matrix({}, 0, 0);
            double r = name == QLatin1String("sum") ? 0 : all[0];
            for (double x : all) r = name == QLatin1String("sum") ? r + x : name == QLatin1String("min") ? std::min(r, x) : std::max(r, x);
            return scalar(r);
        }
        if (name == QLatin1String("zeros") || name == QLatin1String("ones")) {
            const int r = args.empty() ? 1 : int(num(0)), c = args.size() > 1 ? int(num(1)) : r;
            if (r < 0 || c < 0 || double(r) * c > 1e7) raise(tr("%1's size").arg(name), line);
            return Value::matrix(std::vector<double>(std::size_t(r * c), name == QLatin1String("ones") ? 1.0 : 0.0), r, c);
        }
        if (name == QLatin1String("struct")) {
            Value v;
            v.kind = Value::Struct;
            return v;
        }
        if (name == QLatin1String("cell")) {
            Value v;
            v.kind = Value::Cell;
            v.cell.resize(std::size_t(std::clamp(args.empty() ? 0 : int(num(0)), 0, 100000)));
            return v;
        }
        if (name == QLatin1String("isfield")) return scalar(arg(0).field(arg(1).toText()) != nullptr);
        if (name == QLatin1String("rectilineardataset")) {
            Value v;
            v.kind = Value::Dataset;
            v.s = args.empty() ? QString() : arg(0).toText();
            const char* axes[3] = {"x", "y", "z"};
            for (std::size_t i = 1; i < args.size() && i < 4; ++i) v.fields[QString::fromLatin1(axes[i - 1])] = args[i];
            return v;
        }
        // The objects.
        static const std::map<QString, QString> adds = {
            {QStringLiteral("addrect"), QStringLiteral("rect")},
            {QStringLiteral("addpoly"), QStringLiteral("poly")},
            {QStringLiteral("addcircle"), QStringLiteral("circle")},
            {QStringLiteral("addfde"), QStringLiteral("fde")},
            {QStringLiteral("addchargesolver"), QStringLiteral("charge")},
            {QStringLiteral("addsimulationregion"), QStringLiteral("region")},
            {QStringLiteral("adddope"), QStringLiteral("dope")},
            {QStringLiteral("adddiffusion"), QStringLiteral("diffusion")},
            {QStringLiteral("addimportdope"), QStringLiteral("importdope")},
            {QStringLiteral("addelectricalcontact"), QStringLiteral("contact")},
            {QStringLiteral("addmodelmaterial"), QStringLiteral("modelmaterial")},
            {QStringLiteral("addmesh"), QStringLiteral("mesh")}};
        if (const auto it = adds.find(name); it != adds.end()) {
            static const std::map<QString, QString> defaults = {
                {QStringLiteral("rect"), QStringLiteral("rectangle")},  {QStringLiteral("poly"), QStringLiteral("polygon")},
                {QStringLiteral("circle"), QStringLiteral("circle")},   {QStringLiteral("fde"), QStringLiteral("FDE")},
                {QStringLiteral("charge"), QStringLiteral("CHARGE")},   {QStringLiteral("region"), QStringLiteral("simulation region")},
                {QStringLiteral("dope"), QStringLiteral("constant doping")}, {QStringLiteral("diffusion"), QStringLiteral("diffusion")},
                {QStringLiteral("importdope"), QStringLiteral("import doping")}, {QStringLiteral("contact"), QStringLiteral("contact")},
                {QStringLiteral("modelmaterial"), QStringLiteral("New Material")}, {QStringLiteral("mesh"), QStringLiteral("mesh")}};
            add(it->second, defaults.at(it->second));
            return Value();
        }
        if (name == QLatin1String("set")) {
            Object* o = current(line, "set");
            o->props[arg(0).toText()] = arg(1);
            o->history.emplace_back(arg(0).toText(), arg(1));
            return Value();
        }
        if (name == QLatin1String("setnamed")) {
            const int i = named(arg(0).toText());
            if (i < 0) raise(tr("setnamed: there is no %1").arg(arg(0).toText()), line);
            a_scene.objects[std::size_t(i)].props[arg(1).toText()] = arg(2);
            a_scene.objects[std::size_t(i)].history.emplace_back(arg(1).toText(), arg(2));
            return Value();
        }
        if (name == QLatin1String("select")) {
            const int i = named(arg(0).toText());
            if (i < 0) raise(tr("select: there is no %1").arg(arg(0).toText()), line);
            a_current = i;
            return Value();
        }
        if (name == QLatin1String("get") || name == QLatin1String("getnamed")) {
            const Object* o = name == QLatin1String("get") ? current(line, "get") : nullptr;
            if (!o) {
                const int i = named(arg(0).toText());
                if (i < 0) raise(tr("getnamed: there is no %1").arg(arg(0).toText()), line);
                o = &a_scene.objects[std::size_t(i)];
            }
            const Value* v = o->prop(arg(name == QLatin1String("get") ? 0 : 1).toText());
            if (!v) raise(tr("%1: no %2").arg(name, arg(name == QLatin1String("get") ? 0 : 1).toText()), line);
            return *v;
        }
        if (name == QLatin1String("setanalysis")) {
            if (a_fde < 0) raise(tr("setanalysis: there is no FDE"), line);
            a_scene.objects[std::size_t(a_fde)].props[QStringLiteral("analysis/") + arg(0).toText()] = arg(1);
            return Value();
        }
        if (name == QLatin1String("addmaterialproperties")) {
            Object* o = current(line, "addmaterialproperties");
            o->props[QStringLiteral("@") + arg(0).toText()] = arg(1);
            return Value();
        }
        if (name == QLatin1String("addsemiconductorfromalloy")) {
            Object* o = current(line, "addsemiconductorfromalloy");
            o->props[QStringLiteral("@alloy")] = arg(0);
            if (args.size() > 1) o->props[QStringLiteral("@x")] = args[1];
            if (args.size() > 2) o->props[QStringLiteral("@y")] = args[2];
            return Value();
        }
        if (name == QLatin1String("importdataset")) {
            Object* o = current(line, "importdataset");
            o->props[QStringLiteral("@dataset")] = arg(0);
            return Value();
        }
        if (name == QLatin1String("addmaterial")) {
            Material m;
            m.type = args.empty() ? QStringLiteral("(n,k) Material") : arg(0).toText();
            int k = 1;
            while (material(QStringLiteral("New material %1").arg(k))) ++k;
            m.name = QStringLiteral("New material %1").arg(k);
            a_scene.materials.push_back(m);
            return Value::text(m.name);
        }
        if (name == QLatin1String("setmaterial")) {
            Material* m = material(arg(0).toText());
            if (!m) raise(tr("setmaterial: there is no material %1").arg(arg(0).toText()), line);
            const QString key = arg(1).toText().toLower();
            const Value& v = arg(2);
            if (key == QLatin1String("name")) m->name = v.toText();
            else if (key == QLatin1String("anisotropy")) m->anisotropic = v.toNumber() != 0;
            else if (key == QLatin1String("refractive index") || key == QLatin1String("imaginary refractive index")) {
                double* out = key == QLatin1String("refractive index") ? m->n : m->k;
                for (int c = 0; c < 3; ++c) out[c] = v.m.empty() ? 0 : v.m[std::min(v.m.size() - 1, std::size_t(c))];
            } else if (key == QLatin1String("mesh order")) m->priority = int(v.toNumber());
            else if (key == QLatin1String("permittivity")) {
                for (int c = 0; c < 3; ++c) m->n[c] = std::sqrt(std::max(0.0, v.m.empty() ? 1 : v.m[std::min(v.m.size() - 1, std::size_t(c))]));
            }
            return Value();
        }
        if (name == QLatin1String("materialexists")) return scalar(material(arg(0).toText()) != nullptr);
        if (name == QLatin1String("getnamednumber")) return scalar(named(arg(0).toText()) >= 0 ? 1 : 0);
        if (name == QLatin1String("mqwindex") || name == QLatin1String("mqwgain")) {
            // (Its inputs; each call's field - a sweep's.)
            if (a_scene.stack.kind == Value::Null) {
                a_scene.stack = arg(0);
                a_scene.params = arg(1);
                if (args.size() > 2) a_scene.config = args[2];
            }
            if (const Value* V = arg(1).field(QStringLiteral("V")); V && V->kind == Value::Matrix && V->rows >= 2 && V->cols == 2) {
                const double z0 = V->m[0], z1 = V->m[std::size_t(V->rows - 1)];
                const double v0 = V->m[std::size_t(V->rows)], v1 = V->m[std::size_t(2 * V->rows - 1)];
                a_scene.fields.push_back(z1 > z0 ? (v0 - v1) / (z1 - z0) : 0.0);
            }
            Value r;
            r.kind = Value::Struct;
            for (const char* f : {"banddiagram", "bandstructure", "wavefunction", "ome", "emission", "index"}) r.fields[QString::fromLatin1(f)] = Value();
            return r;
        }
        static const QStringList passed = {
            QStringLiteral("newproject"), QStringLiteral("deleteall"),    QStringLiteral("redrawoff"),  QStringLiteral("redrawon"),
            QStringLiteral("redraw"),     QStringLiteral("switchtolayout"), QStringLiteral("save"),    QStringLiteral("run"),
            QStringLiteral("runanalysis"), QStringLiteral("findmodes"),  QStringLiteral("frequencysweep"), QStringLiteral("selectmode"),
            QStringLiteral("savedata"),   QStringLiteral("matlabsave"),  QStringLiteral("visualize"),  QStringLiteral("plot"),
            QStringLiteral("image"),      QStringLiteral("print"),       QStringLiteral("clear"),      QStringLiteral("closeall"),
            QStringLiteral("addtogroup"), QStringLiteral("unselectall"), QStringLiteral("selectall"),  QStringLiteral("legend"),
            QStringLiteral("setplot"),    QStringLiteral("pause"),       QStringLiteral("cleardcard"), QStringLiteral("format"),
            QStringLiteral("getdata"),    QStringLiteral("getresult"),   QStringLiteral("havedata"),   QStringLiteral("feval")};
        if (passed.contains(name)) {
            if (name == QLatin1String("deleteall") || name == QLatin1String("newproject")) {
                a_scene.objects.clear();
                a_current = a_fde = -1;
            }
            if (name == QLatin1String("findmodes")) return scalar(0);
            return Value();
        }
        if (a_tries > 0) raise(tr("%1 is not known").arg(name), line);
        if (!a_unknown.contains(name)) a_unknown << name;
        return Value();
    }
};

} // namespace

QString Value::toText() const
{
    switch (kind) {
    case Text: return s;
    case Matrix: return m.size() == 1 ? QString::number(m[0], 'g', 12) : QString();
    default: return QString();
    }
}

Scene readScript(const QString& text, const QString& source)
{
    Scene scene;
    scene.source = source;
    QString why;
    std::vector<Token> tokens = tokenize(text, &why);
    if (!why.isEmpty()) {
        scene.error = why;
        return scene;
    }
    Parser p(std::move(tokens));
    const std::vector<StmtP> program = p.program();
    if (!p.error.isEmpty()) {
        scene.error = p.error;
        return scene;
    }
    Interpreter(scene).run(program);
    return scene;
}

} // namespace qucs_s::fem::lumerical
