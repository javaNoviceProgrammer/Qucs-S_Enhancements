/***************************************************************************
                              syntax.cpp
                             ------------
    begin                : Sat Mar 11 2006
    copyright            : (C) 2006 by Michael Margraf
    email                : michael.margraf@alumni.tu-berlin.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

// *****************************************************************
// *********                                              **********
// *********  The class that does the syntax highlighting **********
// *********                                              **********
// *****************************************************************

#include "syntax.h"
#include "ink.h"
#include "main.h"
#include "settings.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextDocument>
#include <algorithm>
#include <iterator>

using qucs_s::syntax::Format;
using qucs_s::syntax::Style;

namespace {

// A pattern and the style of what it matches. The rules of a language are
// applied in order, each over the ones before; then its spans (strings and
// comments) over them, then the rules that go over the spans.
struct Rule {
  QRegularExpression pattern;
  Style style;
  int group = 0;            // the part of the match highlighted
  bool overSpans = false;   // over strings and comments
};

// A string or a comment: from its opening mark to its closing one (to
// the end of the line when it has none). Where two start at one place,
// the longer opening mark is taken (''' before ').
struct Span {
  QString open, close;
  Style style;
  bool escapes = false;          // a backslash takes the next character
  bool multiline = false;        // goes on over the next lines until closed
  bool atLineStart = false;      // only first on its line (after blanks)
  bool afterSpace = false;       // only at the start of a line or after a blank
  bool notAfterOperand = false;  // not after a name, a number or a closing bracket (a transpose ')
};

struct StyleUse {
  Style style;
  const char* name;   // in this language; nullptr: the style's own name
};

struct Language {
  int id;
  const char* key;
  const char* name;
  QStringList suffixes;
  QList<StyleUse> styles;
  QList<Rule> rules;
  QList<Span> spans;
};

QRegularExpression pattern(const QString& text, bool anyCase = false)
{
  return QRegularExpression(text, anyCase ? QRegularExpression::CaseInsensitiveOption
                                          : QRegularExpression::NoPatternOption);
}

// Any of the words (separated by blanks), whole.
QRegularExpression words(const char* list, bool anyCase = false)
{
  QStringList alternatives;
  for (const QString& word : QString::fromLatin1(list).split(' ', Qt::SkipEmptyParts))
    alternatives << QRegularExpression::escape(word);
  return pattern("\\b(?:" + alternatives.join('|') + ")\\b", anyCase);
}

Rule rule(const QRegularExpression& re, Style style, int group = 0, bool overSpans = false)
{
  return Rule{re, style, group, overSpans};
}

Span lineSpan(const char* open, Style style)
{
  Span s;
  s.open = QString::fromLatin1(open);
  s.style = style;
  return s;
}

Span span(const char* open, const char* close, Style style, bool escapes = false, bool multiline = false)
{
  Span s;
  s.open = QString::fromLatin1(open);
  s.close = QString::fromLatin1(close);
  s.style = style;
  s.escapes = escapes;
  s.multiline = multiline;
  return s;
}

// Numbers: decimal (1, 1.5, .5, 1e-3), hexadecimal, binary; C's and
// Python's suffixes.
const char* kNumber =
    R"((?<![\w.])(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)[uUlLfFjJ]*(?![\w.]))";

const char* kVhdlKeywords =
    "abs access after alias all and architecture array assert attribute begin block body buffer bus "
    "case component configuration constant context disconnect downto else elsif end entity exit file for "
    "force function generate generic group guarded if impure in inertial inout is label library linkage "
    "literal loop map mod nand new next nor not null of on open or others out package parameter port "
    "postponed procedure process protected pure range record register reject release rem report return "
    "rol ror select severity shared signal sla sll sra srl subtype then to transport type unaffected "
    "units until use variable wait when while with xnor xor";
const char* kVhdlTypes =
    "bit bit_vector boolean boolean_vector character integer integer_vector natural positive real "
    "real_vector signed std_logic std_logic_vector std_ulogic std_ulogic_vector string time "
    "time_vector unsigned line text";

const char* kVerilogKeywords =
    "always always_comb always_ff always_latch and assign automatic begin buf bufif0 bufif1 case casex "
    "casez cell cmos config deassign default defparam design disable edge else end endcase endconfig "
    "endfunction endgenerate endinterface endmodule endpackage endprimitive endspecify endtable endtask "
    "enum event for force forever fork function generate genvar highz0 highz1 if ifnone import incdir "
    "include initial inout input instance interface join large liblist library localparam macromodule "
    "medium module nand negedge nmos nor noshowcancelled not notif0 notif1 or output package parameter "
    "pmos posedge primitive pull0 pull1 pulldown pullup pulsestyle_ondetect pulsestyle_onevent rcmos "
    "release repeat return rnmos rpmos rtran rtranif0 rtranif1 scalared showcancelled signed small "
    "specify specparam strong0 strong1 struct supply0 supply1 table task tran tranif0 tranif1 typedef "
    "unsigned use vectored wait weak0 weak1 while xnor xor";
const char* kVerilogTypes =
    "bit byte int integer logic longint real realtime reg shortint time tri tri0 tri1 triand trior "
    "trireg uwire wand wire wor";

const char* kVerilogAKeywords =
    "aliasparam analog begin branch case continuous default discipline discrete domain else end "
    "endcase enddiscipline endfunction endmodule endnature exclude flow for from function generate "
    "genvar ground if inf inout input localparam module nature output parameter potential repeat "
    "units while";
const char* kVerilogATypes = "electrical integer real string wreal";
const char* kVerilogAFunctions =
    "abs abstol access acos acosh ac_stim analysis asin asinh atan atan2 atanh bound_step ceil cos "
    "cosh cross ddt ddt_nature ddx delay exp final_step flicker_noise floor hypot idt idt_nature idtmod "
    "initial_step laplace_nd laplace_np laplace_zd laplace_zp last_crossing limexp ln log max min "
    "noise_table pow sin sinh slew sqrt tan tanh timer transition white_noise zi_nd zi_np zi_zd zi_zp";

const char* kOctaveKeywords =
    "break case catch classdef continue do else elseif end end_try_catch end_unwind_protect "
    "endclassdef endenumeration endevents endfor endfunction endif endmethods endparfor endproperties "
    "endswitch endwhile enumeration events for function global if methods otherwise parfor "
    "persistent properties return switch try until unwind_protect unwind_protect_cleanup while";
const char* kOctaveBuiltins =
    "abs angle axis conj cos disp eps error exp false figure fprintf grid hold i imag Inf inf "
    "isempty j legend length linspace log log10 logspace max mean min NaN nan numel ones pi plot "
    "printf real semilogx semilogy sin size sprintf sqrt subplot sum tan title true xlabel ylabel zeros";

const char* kSpiceKeywords =
    "ac alter altermod am dc dec destroy display echo exp foreach let lin linearize meas measure "
    "nmos njf npn oct off op param params pjf plot pmos pnp print pulse pwl quit repeat run set "
    "setplot sffm shell show sin source tran trnoise trrandom uic vdmos wrdata write";

const char* kPythonKeywords =
    "False None True and as assert async await break case class continue def del elif else except "
    "finally for from global if import in is lambda match nonlocal not or pass raise return try while "
    "with yield";
const char* kPythonBuiltins =
    "abs all any bin bool bytearray bytes callable chr classmethod cls compile complex delattr dict "
    "dir divmod enumerate eval exec filter float format frozenset getattr globals hasattr hash help "
    "hex id input int isinstance issubclass iter len list locals map max memoryview min next object "
    "oct open ord pow print property range repr reversed round self set setattr slice sorted "
    "staticmethod str sum super tuple type vars zip __init__ __name__ __main__";

const char* kCppKeywords =
    "alignas alignof and asm auto break case catch class const consteval constexpr constinit "
    "const_cast continue co_await co_return co_yield decltype default delete do dynamic_cast else "
    "enum explicit export extern false final for friend goto if inline mutable namespace new noexcept "
    "not nullptr operator or override private protected public register reinterpret_cast requires "
    "return sizeof static static_assert static_cast struct switch template this thread_local throw "
    "true try typedef typeid typename union using virtual volatile while xor";
const char* kCppTypes =
    "bool char char8_t char16_t char32_t double float int int8_t int16_t int32_t int64_t long "
    "ptrdiff_t short signed size_t ssize_t std string uint8_t uint16_t uint32_t uint64_t unsigned "
    "vector void wchar_t";

const char* kShellKeywords =
    "alias break case continue declare do done elif else esac exit export fi for function if in "
    "local readonly return select shift source then time typeset until unset while";
const char* kShellBuiltins =
    "awk cat cd chmod cp echo eval exec false find grep kill ls mkdir mv printf pwd read rm sed "
    "set test trap true wait";

QList<Language> build()
{
  QList<Language> table;
  table.reserve(LANG_COUNT);

  table.append({LANG_NONE, "plain", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Plain Text"), {}, {}, {}, {}});

  {
    Language vhdl{LANG_VHDL, "vhdl", QT_TRANSLATE_NOOP("SyntaxHighlighter", "VHDL"), {"vhd", "vhdl"},
                  {{Style::Keyword, nullptr}, {Style::Type, nullptr}, {Style::Attribute, nullptr},
                   {Style::Number, nullptr}, {Style::Unit, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Time unit")},
                   {Style::String, nullptr}, {Style::Comment, nullptr}},
                  {}, {}};
    vhdl.rules = {
      rule(words(kVhdlKeywords, true), Style::Keyword),
      rule(words(kVhdlTypes, true), Style::Type),
      rule(pattern("'(?:active|ascending|base|delayed|event|high|image|last_active|last_event|last_value|left|"
                   "leftof|length|low|pos|pred|quiet|range|reverse_range|right|rightof|stable|succ|"
                   "transaction|val|value)\\b", true), Style::Attribute),
      rule(pattern(kNumber), Style::Number),
      rule(pattern("\\b\\d+(?:\\.\\d+)?\\s*(fs|ps|ns|us|ms|sec|min|hr)\\b", true), Style::Unit, 1),
      rule(pattern("'.'"), Style::String),   // a character
    };
    vhdl.spans = {lineSpan("--", Style::Comment), span("/*", "*/", Style::Comment, false, true),
                  span("\"", "\"", Style::String)};
    table.append(vhdl);
  }
  {
    Language verilog{LANG_VERILOG, "verilog", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Verilog"),
                     {"v", "vh", "sv", "svh"},
                     {{Style::Keyword, nullptr}, {Style::Type, nullptr},
                      {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "System task")},
                      {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Compiler directive")},
                      {Style::Number, nullptr}, {Style::String, nullptr}, {Style::Comment, nullptr}},
                     {}, {}};
    verilog.rules = {
      rule(words(kVerilogKeywords), Style::Keyword),
      rule(words(kVerilogTypes), Style::Type),
      rule(pattern("\\$\\w+"), Style::Builtin),
      rule(pattern("`\\w+"), Style::Directive),
      rule(pattern("(?<![\\w.])(?:\\d*'[sS]?[bBoOdDhH]\\s*[0-9a-fA-FxXzZ_?]+|\\d[\\d_]*(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)(?![\\w.])"),
           Style::Number),
    };
    verilog.spans = {lineSpan("//", Style::Comment), span("/*", "*/", Style::Comment, false, true),
                     span("\"", "\"", Style::String, true)};
    table.append(verilog);
  }
  {
    Language va{LANG_VERILOGA, "veriloga", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Verilog-A"), {"va", "vams"},
                {{Style::Keyword, nullptr}, {Style::Type, nullptr},
                 {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Function")},
                 {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Compiler directive")},
                 {Style::Number, nullptr}, {Style::Unit, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Scale factor")},
                 {Style::String, nullptr}, {Style::Comment, nullptr}},
                {}, {}};
    va.rules = {
      rule(words(kVerilogAKeywords), Style::Keyword),
      rule(words(kVerilogATypes), Style::Type),
      rule(words(kVerilogAFunctions), Style::Builtin),
      rule(pattern("\\$\\w+"), Style::Builtin),
      rule(pattern("\\b[VI](?=\\s*\\()"), Style::Builtin),   // the access functions
      rule(pattern("`\\w+"), Style::Directive),
      rule(pattern("(?<![\\w.])(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?(?=[TGMKkmunpfa]?(?![\\w.]))"), Style::Number),
      rule(pattern("(?<![\\w.])(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?([TGMKkmunpfa])(?![\\w.])"), Style::Unit, 1),
    };
    va.spans = {lineSpan("//", Style::Comment), span("/*", "*/", Style::Comment, false, true),
                span("\"", "\"", Style::String, true)};
    table.append(va);
  }
  {
    Language octave{LANG_OCTAVE, "octave", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Octave/MATLAB"), {"m", "oct"},
                    {{Style::Keyword, nullptr},
                     {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Function, constant")},
                     {Style::Number, nullptr}, {Style::String, nullptr}, {Style::Comment, nullptr}},
                    {}, {}};
    octave.rules = {
      rule(words(kOctaveKeywords), Style::Keyword),
      rule(words(kOctaveBuiltins), Style::Builtin),
      rule(pattern(kNumber), Style::Number),
    };
    Span block = span("%{", "%}", Style::Comment, false, true);
    block.atLineStart = true;
    Span hashBlock = span("#{", "#}", Style::Comment, false, true);
    hashBlock.atLineStart = true;
    Span quote = span("'", "'", Style::String);
    quote.notAfterOperand = true;   // a' is a transpose
    octave.spans = {block, hashBlock, lineSpan("%", Style::Comment), lineSpan("#", Style::Comment),
                    span("\"", "\"", Style::String, true), quote};
    table.append(octave);
  }
  {
    Language spice{LANG_SPICE, "spice", QT_TRANSLATE_NOOP("SyntaxHighlighter", "SPICE"),
                   {"cir", "ckt", "sp", "spi", "spc", "spice", "lib", "mod", "inc", "sub"},
                   {{Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Dot command")},
                    {Style::Element, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Element name")},
                    {Style::Keyword, nullptr},
                    {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Function")},
                    {Style::Number, nullptr},
                    {Style::Variable, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Expression")},
                    {Style::String, nullptr}, {Style::Comment, nullptr}},
                   {}, {}};
    spice.rules = {
      rule(pattern("^\\s*([A-Za-z][^\\s(]*)"), Style::Element, 1),
      rule(words(kSpiceKeywords, true), Style::Keyword),   // over an element name: a command of .control
      rule(pattern("^\\s*(\\.\\w+)"), Style::Directive, 1),   // over a keyword: .tran
      rule(pattern("\\b(?:abs|atan|avg|ceil|cos|db|deriv|exp|floor|i|imag|int|limit|ln|log|log10|mag|max|mean|"
                   "min|ph|pow|pwr|real|sgn|sin|sqrt|tan|ternary_fcn|v|vdb|vi|vm|vp|vph|vr)(?=\\s*\\()", true),
           Style::Builtin),
      rule(pattern("(?<![\\w.])[+-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?[A-Za-z]*(?![\\w.])"), Style::Number),
      rule(pattern("\\{[^}]*\\}"), Style::Variable),
    };
    Span star = lineSpan("*", Style::Comment);
    star.atLineStart = true;
    spice.spans = {star, lineSpan(";", Style::Comment), lineSpan("$ ", Style::Comment),
                   span("\"", "\"", Style::String)};
    table.append(spice);
  }
  {
    Language net{LANG_QUCS_NETLIST, "qucsnetlist", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Qucs Netlist"), {"net"},
                 {{Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Definition, simulation")},
                  {Style::Keyword, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Component type")},
                  {Style::Element, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Instance name")},
                  {Style::Key, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Property")},
                  {Style::String, nullptr}, {Style::Comment, nullptr}},
                 {}, {}};
    net.rules = {
      rule(pattern("^\\s*([A-Za-z_]\\w*)(?=:)"), Style::Keyword, 1),
      rule(pattern("^\\s*(\\.\\w+)"), Style::Directive, 1),
      rule(pattern("^\\s*\\.?\\w+:(\\S+)"), Style::Element, 1),
      rule(pattern("\\b([A-Za-z_]\\w*)(?==)"), Style::Key, 1),
    };
    net.spans = {lineSpan("#", Style::Comment), span("\"", "\"", Style::String)};
    table.append(net);
  }
  {
    Language python{LANG_PYTHON, "python", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Python"), {"py", "pyw"},
                    {{Style::Keyword, nullptr},
                     {Style::Builtin, nullptr},
                     {Style::Element, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Definition")},
                     {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Decorator")},
                     {Style::Number, nullptr}, {Style::String, nullptr}, {Style::Comment, nullptr}},
                    {}, {}};
    python.rules = {
      rule(words(kPythonKeywords), Style::Keyword),
      rule(words(kPythonBuiltins), Style::Builtin),
      rule(pattern("\\b(?:def|class)\\s+(\\w+)"), Style::Element, 1),
      rule(pattern("^\\s*(@[\\w.]+)"), Style::Directive, 1),
      rule(pattern(kNumber), Style::Number),
    };
    python.spans = {span("\"\"\"", "\"\"\"", Style::String, true, true), span("'''", "'''", Style::String, true, true),
                    span("\"", "\"", Style::String, true), span("'", "'", Style::String, true),
                    lineSpan("#", Style::Comment)};
    table.append(python);
  }
  {
    Language cpp{LANG_CPP, "cpp", QT_TRANSLATE_NOOP("SyntaxHighlighter", "C/C++"),
                 {"c", "h", "cc", "cpp", "cxx", "c++", "hpp", "hh", "hxx", "h++"},
                 {{Style::Keyword, nullptr}, {Style::Type, nullptr},
                  {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Preprocessor")},
                  {Style::Number, nullptr}, {Style::String, nullptr}, {Style::Comment, nullptr}},
                 {}, {}};
    cpp.rules = {
      rule(words(kCppKeywords), Style::Keyword),
      rule(words(kCppTypes), Style::Type),
      rule(pattern(kNumber), Style::Number),
      rule(pattern("^\\s*#\\s*\\w+"), Style::Directive),
      rule(pattern("^\\s*#\\s*include\\s*(<[^>]*>)"), Style::String, 1),
    };
    cpp.spans = {lineSpan("//", Style::Comment), span("/*", "*/", Style::Comment, false, true),
                 span("\"", "\"", Style::String, true), span("'", "'", Style::String, true)};
    table.append(cpp);
  }
  {
    Language shell{LANG_SHELL, "shell", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Shell Script"), {"sh", "bash", "zsh"},
                   {{Style::Keyword, nullptr},
                    {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Command")},
                    {Style::Variable, nullptr},
                    {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Interpreter line")},
                    {Style::String, nullptr}, {Style::Comment, nullptr}},
                   {}, {}};
    shell.rules = {
      rule(words(kShellKeywords), Style::Keyword),
      rule(words(kShellBuiltins), Style::Builtin),
      rule(pattern("\\$\\{[^}]*\\}|\\$[A-Za-z_]\\w*|\\$[0-9#?@*$!-]"), Style::Variable),
      rule(pattern("^#!.*"), Style::Directive, 0, true),
    };
    Span hash = lineSpan("#", Style::Comment);
    hash.afterSpace = true;   // not the # of ${#list} or $#
    shell.spans = {hash, span("\"", "\"", Style::String, true), span("'", "'", Style::String)};
    table.append(shell);
  }
  {
    Language markdown{LANG_MARKDOWN, "markdown", QT_TRANSLATE_NOOP("SyntaxHighlighter", "Markdown"), {"md", "markdown"},
                      {{Style::Heading, nullptr}, {Style::Emphasis, nullptr}, {Style::Link, nullptr},
                       {Style::Code, nullptr},
                       {Style::Keyword, QT_TRANSLATE_NOOP("SyntaxHighlighter", "List marker")},
                       {Style::Comment, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Quote, comment")}},
                      {}, {}};
    markdown.rules = {
      rule(pattern("^\\s{0,3}(?:[-*+]|\\d+[.)])(?=\\s)"), Style::Keyword),
      rule(pattern("(\\*\\*|__)(?=\\S).+?(?<=\\S)\\1"), Style::Emphasis),
      rule(pattern("(?<![*\\w])\\*(?=[^\\s*])[^*]*?(?<=[^\\s*])\\*(?![*\\w])"), Style::Emphasis),
      rule(pattern("(?<![_\\w])_(?=[^\\s_])[^_]*?(?<=[^\\s_])_(?![_\\w])"), Style::Emphasis),
      rule(pattern("!?\\[[^\\]]*\\]\\([^)]*\\)|<https?://[^>]+>|https?://[^\\s)>]+"), Style::Link),
      rule(pattern("^\\s{0,3}>.*"), Style::Comment),
      rule(pattern("^\\s{0,3}#{1,6}(?:\\s.*)?$"), Style::Heading),
      rule(pattern("^(?:=+|-{2,})\\s*$"), Style::Heading),
    };
    markdown.spans = {span("```", "```", Style::Code, false, true), span("~~~", "~~~", Style::Code, false, true),
                      span("`", "`", Style::Code), span("<!--", "-->", Style::Comment, false, true)};
    table.append(markdown);
  }
  {
    Language json{LANG_JSON, "json", QT_TRANSLATE_NOOP("SyntaxHighlighter", "JSON"), {"json"},
                  {{Style::Key, nullptr}, {Style::String, nullptr}, {Style::Number, nullptr},
                   {Style::Keyword, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Literal")}},
                  {}, {}};
    json.rules = {
      rule(words("true false null"), Style::Keyword),
      rule(pattern("(?<![\\w.])-?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?\\d+)?(?![\\w.])"), Style::Number),
      rule(pattern("\"(?:[^\"\\\\]|\\\\.)*\"(?=\\s*:)"), Style::Key, 0, true),
    };
    json.spans = {span("\"", "\"", Style::String, true)};
    table.append(json);
  }
  {
    Language xml{LANG_XML, "xml", QT_TRANSLATE_NOOP("SyntaxHighlighter", "XML"), {"xml", "xsd", "xsl", "xslt"},
                 {{Style::Tag, nullptr}, {Style::Attribute, nullptr},
                  {Style::String, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Attribute value")},
                  {Style::Builtin, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Entity")},
                  {Style::Directive, QT_TRANSLATE_NOOP("SyntaxHighlighter", "Declaration")},
                  {Style::Comment, nullptr}},
                 {}, {}};
    xml.rules = {
      rule(pattern("</?[\\w:.-]+|/?>"), Style::Tag),
      rule(pattern("([\\w:.-]+)(?=\\s*=\\s*[\"'])"), Style::Attribute, 1),
      rule(pattern("=\\s*(\"[^\"]*\"|'[^']*')"), Style::String, 1),
      rule(pattern("&(?:#\\d+|#x[0-9a-fA-F]+|\\w+);"), Style::Builtin),
      rule(pattern("<\\?.*?\\?>|<!DOCTYPE[^>]*>", true), Style::Directive),
    };
    xml.spans = {span("<!--", "-->", Style::Comment, false, true),
                 span("<![CDATA[", "]]>", Style::String, false, true)};
    table.append(xml);
  }
  return table;
}

const Language& definition(int language)
{
  static const QList<Language> table = build();
  Q_ASSERT(table.size() == LANG_COUNT);
  if (language < 0 || language >= table.size()) return table.first();
  Q_ASSERT(table[language].id == language);
  return table[language];
}

const char* kStyleKeys[] = {"Keyword", "Type", "Builtin", "Directive", "Element", "Variable", "Number", "String",
                            "Unit", "Comment", "Heading", "Emphasis", "Code", "Link", "Tag", "Attribute", "Key"};
static_assert(std::size(kStyleKeys) == size_t(Style::Count));

const char* kStyleNames[] = {
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Keyword"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Data type"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Built-in"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Directive"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Name"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Variable"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Number"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "String"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Unit"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Comment"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Heading"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Emphasis"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Code"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Link"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Tag"), QT_TRANSLATE_NOOP("SyntaxHighlighter", "Attribute"),
  QT_TRANSLATE_NOOP("SyntaxHighlighter", "Key"),
};
static_assert(std::size(kStyleNames) == size_t(Style::Count));

QString tr(const char* text)
{
  return QCoreApplication::translate("SyntaxHighlighter", text);
}

// Where a span's opening mark is, from \a from on; -1 when it is not there.
int findOpen(const QString& text, int from, const Span& s)
{
  for (int pos = text.indexOf(s.open, from); pos >= 0; pos = text.indexOf(s.open, pos + 1)) {
    if (s.atLineStart && !QStringView(text).left(pos).trimmed().isEmpty()) continue;
    if (s.afterSpace && pos > 0 && !text.at(pos - 1).isSpace()) continue;
    if (s.notAfterOperand) {
      int before = pos - 1;
      while (before >= 0 && text.at(before) == ' ') --before;
      if (before >= 0) {
        const QChar c = text.at(before);
        if (c.isLetterOrNumber() || c == '_' || c == ')' || c == ']' || c == '}' || c == '.' || c == '\'') continue;
      }
    }
    return pos;
  }
  return -1;
}

// Where a span ends (after its closing mark), from \a from on; -1 when it
// does not end on this line.
int findClose(const QString& text, int from, const Span& s)
{
  for (int pos = text.indexOf(s.close, from); pos >= 0; pos = text.indexOf(s.close, pos + 1)) {
    if (s.escapes) {
      int backslashes = 0;
      for (int i = pos - 1; i >= from && text.at(i) == '\\'; --i) ++backslashes;
      if (backslashes % 2 == 1) continue;   // escaped
    }
    return pos + int(s.close.size());
  }
  return -1;
}

Format general(Style style)
{
  switch (style) {
  case Style::Keyword:   return {QColor(0x00, 0x00, 0x7f), true, false};
  case Style::Type:      return {QColor(0x26, 0x7f, 0x99), false, false};
  case Style::Builtin:   return {QColor(0x79, 0x5e, 0x26), false, false};
  case Style::Directive: return {QColor(0xaf, 0x00, 0xdb), false, false};
  case Style::Element:   return {QColor(0x00, 0x70, 0xc1), false, false};
  case Style::Variable:  return {QColor(0x00, 0x10, 0x80), false, false};
  case Style::Number:    return {QColor(0x09, 0x86, 0x58), false, false};
  case Style::String:    return {QColor(0xa3, 0x15, 0x15), false, false};
  case Style::Unit:      return {QColor(0x8b, 0x00, 0x00), false, true};
  case Style::Comment:   return {QColor(0x80, 0x80, 0x80), false, true};
  case Style::Heading:   return {QColor(0x00, 0x00, 0x7f), true, false};
  case Style::Emphasis:  return {QColor(0x00, 0x00, 0x00), false, true};
  case Style::Code:      return {QColor(0xa3, 0x15, 0x15), false, false};
  case Style::Link:      return {QColor(0x00, 0x00, 0xee), false, false};
  case Style::Tag:       return {QColor(0x80, 0x00, 0x00), false, false};
  case Style::Attribute: return {QColor(0xe5, 0x00, 0x00), false, false};
  case Style::Key:       return {QColor(0x04, 0x51, 0xa5), false, false};
  case Style::Count:     break;
  }
  return {QColor(Qt::black), false, false};
}

QString formatKey(int language, Style style)
{
  return qucs_s::syntax::key(language) + '/' + qucs_s::syntax::styleKey(style);
}

} // namespace

namespace qucs_s::syntax {

QList<int> languages()
{
  QList<int> list;
  for (int language = LANG_NONE + 1; language < LANG_COUNT; ++language) list.append(language);
  std::sort(list.begin(), list.end(), [](int a, int b) {
    return QString::compare(name(a), name(b), Qt::CaseInsensitive) < 0;
  });
  list.prepend(LANG_NONE);
  return list;
}

QString key(int language)
{
  return QString::fromLatin1(definition(language).key);
}

int byKey(const QString& key)
{
  for (int language = 0; language < LANG_COUNT; ++language)
    if (key == QLatin1String(definition(language).key)) return language;
  return -1;
}

QString name(int language)
{
  return tr(definition(language).name);
}

QStringList defaultSuffixes(int language)
{
  return definition(language).suffixes;
}

int defaultLanguageFor(const QString& suffix)
{
  const QString lower = suffix.toLower();
  if (lower.isEmpty()) return LANG_NONE;
  for (int language = LANG_NONE + 1; language < LANG_COUNT; ++language)
    if (definition(language).suffixes.contains(lower)) return language;
  return LANG_NONE;
}

int languageFor(const QString& fileName)
{
  const QString suffix = QFileInfo(fileName).suffix().toLower();
  if (suffix.isEmpty()) return LANG_NONE;
  const auto chosen = QucsSettings.SyntaxForSuffix.constFind(suffix);
  if (chosen != QucsSettings.SyntaxForSuffix.constEnd()) {
    const int language = byKey(*chosen);
    if (language >= 0) return language;
  }
  return defaultLanguageFor(suffix);
}

void chooseFor(const QString& suffix, int language)
{
  const QString lower = suffix.toLower();
  if (lower.isEmpty() || language < 0 || language >= LANG_COUNT) return;
  if (language == defaultLanguageFor(lower))
    QucsSettings.SyntaxForSuffix.remove(lower);
  else
    QucsSettings.SyntaxForSuffix.insert(lower, key(language));
  saveChoices();
}

bool chosenFor(const QString& suffix)
{
  return QucsSettings.SyntaxForSuffix.contains(suffix.toLower());
}

QStringList suffixesOf(int language)
{
  QStringList suffixes;
  for (const QString& suffix : defaultSuffixes(language))
    if (languageFor("x." + suffix) == language) suffixes.append(suffix);
  QStringList chosen;
  for (auto it = QucsSettings.SyntaxForSuffix.cbegin(); it != QucsSettings.SyntaxForSuffix.cend(); ++it)
    if (byKey(it.value()) == language && !suffixes.contains(it.key())) chosen.append(it.key());
  chosen.sort();
  return suffixes + chosen;
}

QList<Style> styles(int language)
{
  QList<Style> list;
  for (const StyleUse& use : definition(language).styles) list.append(use.style);
  return list;
}

QString styleName(int language, Style style)
{
  for (const StyleUse& use : definition(language).styles)
    if (use.style == style && use.name != nullptr) return tr(use.name);
  return tr(kStyleNames[int(style)]);
}

QString styleKey(Style style)
{
  return QString::fromLatin1(kStyleKeys[int(style)]);
}

Format defaultFormat(int, Style style)
{
  return general(style);
}

Format format(int language, Style style)
{
  const Format fallback = defaultFormat(language, style);
  const auto saved = QucsSettings.SyntaxFormats.constFind(formatKey(language, style));
  return saved != QucsSettings.SyntaxFormats.constEnd() ? parseFormat(*saved, fallback) : fallback;
}

void setFormat(int language, Style style, const Format& format)
{
  if (format == defaultFormat(language, style))
    QucsSettings.SyntaxFormats.remove(formatKey(language, style));
  else
    QucsSettings.SyntaxFormats.insert(formatKey(language, style), formatText(format));
}

QString formatText(const Format& format)
{
  QString text = format.colour.name(QColor::HexRgb);
  if (format.bold) text += QStringLiteral(" bold");
  if (format.italic) text += QStringLiteral(" italic");
  return text;
}

Format parseFormat(const QString& text, const Format& fallback)
{
  const QStringList parts = text.split(' ', Qt::SkipEmptyParts);
  if (parts.isEmpty()) return fallback;
  Format format;
  format.colour = QColor::fromString(parts.first());
  if (!format.colour.isValid()) format.colour = fallback.colour;
  format.bold = parts.contains(QStringLiteral("bold"));
  format.italic = parts.contains(QStringLiteral("italic"));
  return format;
}

QString sample(int language)
{
  switch (language) {
  case LANG_VHDL:
    return QStringLiteral(
        "-- A counter\n"
        "library ieee;\nuse ieee.std_logic_1164.all;\n\n"
        "entity counter is\n  port (clk : in std_logic; q : out integer);\nend entity;\n\n"
        "architecture rtl of counter is\nbegin\n  process (clk)\n    variable n : integer := 0;\n  begin\n"
        "    if clk'event and clk = '1' then\n      n := n + 1;\n    end if;\n"
        "    q <= n after 10 ns;\n    report \"counted\";\n  end process;\nend architecture;\n");
  case LANG_VERILOG:
    return QStringLiteral(
        "`timescale 1ns / 1ps\n// A counter\nmodule counter (input clk, output reg [7:0] q);\n"
        "  always @(posedge clk) begin\n    q <= q + 8'h01;\n    $display(\"q = %d\", q);\n  end\n"
        "endmodule\n");
  case LANG_VERILOGA:
    return QStringLiteral(
        "`include \"disciplines.vams\"\n// A resistor with a temperature coefficient\n"
        "module res(p, n);\n  inout p, n;\n  electrical p, n;\n  parameter real r = 1k;\n  real t;\n"
        "  analog begin\n    t = $temperature - 300.15;\n    I(p, n) <+ V(p, n) / (r * (1 + 2m * t));\n"
        "    $strobe(\"done\");\n  end\nendmodule\n");
  case LANG_OCTAVE:
    return QStringLiteral(
        "% The response of a low-pass\nfunction h = lowpass(f, fc)\n  h = 1 ./ (1 + 1i * f / fc);\nendfunction\n\n"
        "f = logspace(1, 6, 200)';\nplot(f, abs(lowpass(f, 1e3)));\ntitle('Low-pass');\n");
  case LANG_SPICE:
    return QStringLiteral(
        "* An RC low-pass\nV1 in 0 DC 0 AC 1 PULSE(0 5 1n 1n 1n 1u 2u)\nR1 in out 1k ; the resistor\n"
        "C1 out 0 {cval}\n.param cval = 10n\n.include \"models.lib\"\n.tran 10n 5u\n.ac dec 20 1 1meg\n"
        ".control\nrun\nplot v(out) vdb(out)\n.endc\n.end\n");
  case LANG_QUCS_NETLIST:
    return QStringLiteral(
        "# Qucs netlist\n.Def:lowpass _net0 _net1\nR:R1 _net0 _net1 R=\"1 kOhm\" Temp=\"26.85\"\n"
        "C:C1 _net1 gnd C=\"10 nF\"\n.Def:End\n.AC:AC1 Type=\"log\" Start=\"1 Hz\" Stop=\"1 MHz\" Points=\"101\"\n");
  case LANG_PYTHON:
    return QStringLiteral(
        "#!/usr/bin/env python3\n\"\"\"Reads a dataset.\"\"\"\nimport math\n\n@staticmethod\n"
        "def gain(v_out, v_in=1.0):\n    # in decibels\n    return 20 * math.log10(abs(v_out / v_in))\n\n"
        "class Result:\n    def __init__(self, name):\n        self.name = f\"result {name}\"\n\n"
        "print(gain(0.5), len('abc'), None, 0x1F)\n");
  case LANG_CPP:
    return QStringLiteral(
        "#include <vector>\n#define GAIN 2.5\n\n/* The sum of the\n   samples */\ndouble sum(const std::vector<double>& v)\n"
        "{\n    double total = 0.0;   // so far\n    for (auto x : v) total += x * GAIN;\n"
        "    return total > 1e3 ? 0 : total;\n}\n\nconst char* name = \"sum\";\nchar unit = 'V';\n");
  case LANG_SHELL:
    return QStringLiteral(
        "#!/bin/sh\n# Runs every netlist\nfor f in *.cir; do\n  echo \"running $f\"\n"
        "  ngspice -b \"$f\" > \"${f%.cir}.log\"\ndone\nexport COUNT=$#\n");
  case LANG_MARKDOWN:
    return QStringLiteral(
        "# The amplifier\n\nIt has a **gain** of 20 dB and *low* noise.\n\n- see [the schematic](amp.sch)\n"
        "- run `ngspice amp.cir`\n\n> A note.\n\n```\nplot v(out)\n```\n");
  case LANG_JSON:
    return QStringLiteral(
        "{\n  \"name\": \"amplifier\",\n  \"gain\": 20.5,\n  \"stages\": [1, 2],\n  \"stable\": true,\n"
        "  \"notes\": null\n}\n");
  case LANG_XML:
    return QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- A component library -->\n"
        "<library name=\"passives\">\n  <component type=\"R\" value='1k'>A resistor &amp; more</component>\n"
        "  <![CDATA[raw <text>]]>\n</library>\n");
  default:
    return QString();
  }
}

void saveChoices()
{
  QucsSettingsFile settings;
  settings.remove(QStringLiteral("SyntaxForSuffix"));
  settings.beginGroup(QStringLiteral("SyntaxForSuffix"));
  for (auto it = QucsSettings.SyntaxForSuffix.cbegin(); it != QucsSettings.SyntaxForSuffix.cend(); ++it)
    settings.setValue(it.key(), it.value());
  settings.endGroup();
}

} // namespace qucs_s::syntax

// ---------------------------------------------------
SyntaxHighlighter::SyntaxHighlighter(QObject* parent) : QSyntaxHighlighter(parent)
{
  if (auto* editor = qobject_cast<QPlainTextEdit*>(parent))
    setDocument(editor->document());
  else if (auto* document = qobject_cast<QTextDocument*>(parent))
    setDocument(document);
  makeFormats();
}

SyntaxHighlighter::~SyntaxHighlighter() = default;

void SyntaxHighlighter::setLanguage(int language)
{
  a_language = (language >= 0 && language < LANG_COUNT) ? language : LANG_NONE;
  makeFormats();
  rehighlight();
}

void SyntaxHighlighter::setPaper(const QColor& paper)
{
  a_paper = paper;
  makeFormats();
  rehighlight();
}

void SyntaxHighlighter::setFormats(const QHash<int, Format>& formats)
{
  a_own = formats;
  makeFormats();
  rehighlight();
}

void SyntaxHighlighter::reload()
{
  makeFormats();
  rehighlight();
}

void SyntaxHighlighter::makeFormats()
{
  const qucs_s::ink::Paper on(a_paper);
  for (int s = 0; s < int(Style::Count); ++s) {
    const auto own = a_own.constFind(s);
    const Format f = own != a_own.constEnd() ? *own : qucs_s::syntax::format(a_language, Style(s));
    QTextCharFormat format;
    format.setForeground(qucs_s::ink::on(f.colour));
    format.setFontWeight(f.bold ? QFont::Bold : QFont::Normal);
    format.setFontItalic(f.italic);
    a_formats[s] = format;
  }
}

// ---------------------------------------------------
void SyntaxHighlighter::highlightBlock(const QString& text)
{
  setCurrentBlockState(0);
  if (a_language == LANG_NONE) return;
  const Language& language = definition(a_language);
  const auto apply = [this, &text](const Rule& rule) {
    QRegularExpressionMatchIterator it = rule.pattern.globalMatch(text);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      const int start = int(match.capturedStart(rule.group));
      const int length = int(match.capturedLength(rule.group));
      if (start >= 0 && length > 0) setFormat(start, length, a_formats[int(rule.style)]);
    }
  };
  for (const Rule& rule : language.rules)
    if (!rule.overSpans) apply(rule);
  scanSpans(text);
  for (const Rule& rule : language.rules)
    if (rule.overSpans) apply(rule);
}

// The strings and comments, from left to right: whichever starts first
// takes what follows it (a # in a string is not a comment); one that is
// not closed on its line goes on over the next ones when it may (the
// block state is its number + 1).
void SyntaxHighlighter::scanSpans(const QString& text)
{
  const QList<Span>& spans = definition(a_language).spans;
  const int length = int(text.size());
  int from = 0;
  const int carried = previousBlockState() - 1;
  if (carried >= 0 && carried < spans.size()) {
    const Span& open = spans[carried];
    const int end = findClose(text, 0, open);
    if (end < 0) {
      setFormat(0, length, a_formats[int(open.style)]);
      setCurrentBlockState(carried + 1);
      return;
    }
    setFormat(0, end, a_formats[int(open.style)]);
    from = end;
  }
  while (from < length) {
    int first = -1, at = length;
    for (int k = 0; k < spans.size(); ++k) {
      const int pos = findOpen(text, from, spans[k]);
      if (pos < 0) continue;
      if (pos < at || (pos == at && spans[k].open.size() > spans[first].open.size())) {
        first = k;
        at = pos;
      }
    }
    if (first < 0) break;
    const Span& s = spans[first];
    const QTextCharFormat& format = a_formats[int(s.style)];
    if (s.close.isEmpty()) {
      setFormat(at, length - at, format);
      break;
    }
    const int end = findClose(text, at + int(s.open.size()), s);
    if (end < 0) {
      setFormat(at, length - at, format);
      if (s.multiline) setCurrentBlockState(first + 1);
      break;
    }
    setFormat(at, end - at, format);
    from = end;
  }
}
