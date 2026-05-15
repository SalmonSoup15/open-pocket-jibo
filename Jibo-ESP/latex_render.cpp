#include "latex_render.h"
#include "log.h"
#include <Arduino.h>
#include <vector>

// ─── Available fonts (must match lv_conf.h) ─────────────────────────────────
//
// We pick a (main, small) pair based on the natural rendered size.  Smaller
// pairs are tried only if the larger pair overflows max_w/max_h.  All three
// pairs reuse fonts already enabled in lv_conf.h (no extra flash cost).
struct FontPair {
    const lv_font_t *main;
    const lv_font_t *small;
};
// Tried in order — first pair whose natural rendered size fits the
// (max_w, max_h) advisory bounds wins.  Default is 48/24 because the
// round display can comfortably show that for the formulas Gemini
// typically emits (quadratic, area, simple algebra).  Smaller pairs are
// only used as a fallback for unusually wide / tall expressions.
static const FontPair FONT_PAIRS[] = {
    { &lv_font_montserrat_48, &lv_font_montserrat_24 },
    { &lv_font_montserrat_24, &lv_font_montserrat_14 },
    { &lv_font_montserrat_20, &lv_font_montserrat_12 },
    { &lv_font_montserrat_16, &lv_font_montserrat_12 },
};
static const int NUM_FONT_PAIRS = (int)(sizeof(FONT_PAIRS) / sizeof(FONT_PAIRS[0]));

// ─── Expression tree ────────────────────────────────────────────────────────
//
// Built from a single recursive-descent pass over the LaTeX string.  Each
// node owns its children (deleted recursively in ~Node).
struct Node {
    enum Kind {
        Atom, Row, Frac, Sqrt, SupSub,
        Accent,    // a = base, accentKind = which mark to draw above
        Over,      // a = body, with horizontal line above (overline / overrightarrow)
        Under,     // a = body, with horizontal line below (underline)
        Box,       // a = body, framed by rectangle
        Space      // fixed-width gap, "spaceEm" = fraction of em-width
    };
    Kind kind;

    // Atom: rendered text (already substituted for any commands)
    String text;

    // Frac:   a = numerator,    b = denominator
    // Sqrt:   a = argument
    // SupSub: a = base,         b = superscript (or NULL),  c = subscript (or NULL)
    // Accent: a = base
    // Over / Under / Box: a = body
    Node *a = NULL;
    Node *b = NULL;
    Node *c = NULL;

    // Row: ordered children, baseline-aligned horizontally
    std::vector<Node *> row;

    // Accent kind — only meaningful when kind == Accent.
    enum AccentKind {
        AccHat, AccBar, AccVec, AccTilde, AccDot, AccDDot
    };
    AccentKind accent = AccHat;

    // Over kind — distinguishes plain overline from "\overrightarrow"
    // (which gets a tiny arrowhead at the right end of the bar).
    bool overArrow = false;

    // Space size in fractions of an em (1/6 = thin space, 1.0 = quad).
    float spaceEm = 0.0f;

    // Layout (filled by layout())
    int w = 0, h = 0;
    int baseline = 0;   // distance from top of bbox to text baseline

    Node(Kind k) : kind(k) {}
    ~Node() {
        delete a; delete b; delete c;
        for (auto *p : row) delete p;
    }
};

// ─── Parser ─────────────────────────────────────────────────────────────────
//
// Grammar (informal):
//   expression := atom (super_sub_suffix)* (atom (super_sub_suffix)*)* ...
//   atom       := '{' expression '}'
//               | '\' command
//               | ordinary_char
//   super_sub  := ('^' | '_') atom
//   command    := identifier (consumed as far as alphabetic)

static void skip_ws(const String &s, int &p) {
    int n = s.length();
    while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
}

static Node *parse_expression(const String &s, int &p, char until);
static Node *parse_atom(const String &s, int &p);

// Substitute a LaTeX command name with the text we'll render.  Returns
// empty string if the command takes arguments (caller handles those).
//
// Stock Montserrat fonts on this device only carry basic Latin glyphs
// (U+0020-U+007F + the LVGL icon range).  Greek letters, ∑ ∏ ∫ ∞ and
// other math symbols simply aren't in the font, so we use *spelled-out
// ASCII fallbacks*.  This matches the existing convention (`\infty` →
// "inf") and means the user sees something readable instead of a
// missing-glyph box.  Single-char Greek substitutes ("α" → "a") were
// considered and rejected because they collide with normal variables.
//
// Table-driven so the supported list is easy to read and extend.  Order
// doesn't matter; first match wins.
struct Sub { const char *cmd; const char *out; };
static const Sub SUBS[] = {
    // ─── Basic operators / relations (kept from v1) ─────────────────────
    { "pm",        "+/-" },
    { "mp",        "-/+" },
    { "times",     "x" },
    { "div",       "/" },
    { "cdot",      "." },
    { "ast",       "*" },
    { "star",      "*" },
    { "bullet",    "." },
    { "circ",      "o" },
    { "leq",       "<=" },
    { "le",        "<=" },
    { "geq",       ">=" },
    { "ge",        ">=" },
    { "neq",       "!=" },
    { "ne",        "!=" },
    { "approx",    "~" },
    { "sim",       "~" },
    { "simeq",     "~=" },
    { "cong",      "~=" },
    { "equiv",     "==" },
    { "propto",    "prop" },
    { "perp",      "perp" },
    { "parallel",  "||" },
    { "ll",        "<<" },
    { "gg",        ">>" },
    { "prec",      "<" },
    { "succ",      ">" },
    { "preceq",    "<=" },
    { "succeq",    ">=" },

    // ─── Set theory ─────────────────────────────────────────────────────
    { "in",         "in" },
    { "notin",      "!in" },
    { "ni",         "ni" },
    { "subset",     "sub" },
    { "supset",     "sup" },
    { "subseteq",   "sub=" },
    { "supseteq",   "sup=" },
    { "cup",        "U" },
    { "cap",        "n" },
    { "setminus",   "\\" },
    { "emptyset",   "{}" },
    { "varnothing", "{}" },

    // ─── Logic ──────────────────────────────────────────────────────────
    { "forall",  "for all" },
    { "exists",  "exists" },
    { "nexists", "!exists" },
    { "neg",     "!" },
    { "lnot",    "!" },
    { "land",    "and" },
    { "wedge",   "and" },
    { "lor",     "or" },
    { "vee",     "or" },
    { "implies", "=>" },
    { "iff",     "<=>" },
    { "Rightarrow",     "=>" },
    { "Leftarrow",      "<=" },
    { "Leftrightarrow", "<=>" },

    // ─── Arrows ─────────────────────────────────────────────────────────
    { "to",            "->" },
    { "rightarrow",    "->" },
    { "longrightarrow","->" },
    { "gets",          "<-" },
    { "leftarrow",     "<-" },
    { "longleftarrow", "<-" },
    { "leftrightarrow","<->" },
    { "mapsto",        "|->" },
    { "hookrightarrow","->" },
    { "uparrow",       "up" },
    { "downarrow",     "dn" },
    { "Uparrow",       "UP" },
    { "Downarrow",     "DN" },

    // ─── Greek lowercase (spelled out — Montserrat has no Greek) ────────
    { "alpha",   "alpha"   }, { "beta",   "beta"   },
    { "gamma",   "gamma"   }, { "delta",  "delta"  },
    { "epsilon", "epsilon" }, { "varepsilon", "epsilon" },
    { "zeta",    "zeta"    }, { "eta",    "eta"    },
    { "theta",   "theta"   }, { "vartheta", "theta" },
    { "iota",    "iota"    }, { "kappa",  "kappa"  },
    { "lambda",  "lambda"  }, { "mu",     "mu"     },
    { "nu",      "nu"      }, { "xi",     "xi"     },
    { "omicron", "omicron" }, { "pi",     "pi"     }, { "varpi", "pi" },
    { "rho",     "rho"     }, { "varrho", "rho"    },
    { "sigma",   "sigma"   }, { "varsigma", "sigma" },
    { "tau",     "tau"     }, { "upsilon","upsilon"},
    { "phi",     "phi"     }, { "varphi", "phi"    },
    { "chi",     "chi"     }, { "psi",    "psi"    },
    { "omega",   "omega"   },

    // ─── Greek uppercase (the ones whose Latin twin is visually distinct
    //     get spelled out — Α/Β/Ε/Ζ/Η/Ι/Κ/Μ/Ν/Ο/Ρ/Τ/Υ/Χ render as A/B/E/Z/H/I/K/M/N/O/P/T/Y/X)
    { "Gamma",   "Gamma"   }, { "Delta",   "Delta"   },
    { "Theta",   "Theta"   }, { "Lambda",  "Lambda"  },
    { "Xi",      "Xi"      }, { "Pi",      "Pi"      },
    { "Sigma",   "Sigma"   }, { "Upsilon", "Upsilon" },
    { "Phi",     "Phi"     }, { "Psi",     "Psi"     },
    { "Omega",   "Omega"   },

    // ─── Big operators (\sum_{i=1}^{n} still works via subscript/superscript) ─
    { "sum",     "Sum"  },
    { "prod",    "Prod" },
    { "coprod",  "II"   },
    { "int",     "Int"  },
    { "iint",    "Int2" },
    { "iiint",   "Int3" },
    { "oint",    "Oint" },
    { "bigcup",  "U"    },
    { "bigcap",  "n"    },
    { "bigvee",  "or"   },
    { "bigwedge","and"  },
    { "bigoplus","(+)"  },
    { "bigotimes","(x)" },
    { "partial", "d"    },
    { "nabla",   "del"  },

    // ─── Misc symbols ───────────────────────────────────────────────────
    { "infty",   "inf" },
    { "infinity","inf" },
    { "ldots",   "..."  },
    { "dots",    "..."  },
    { "cdots",   "..."  },
    { "vdots",   "."    },
    { "ddots",   "..."  },
    { "prime",   "'"    },
    { "dagger",  "+"    },
    { "ddagger", "++"   },
    { "degree",  "deg"  },
    { "angle",   "/_"   },
    { "triangle","/\\"  },
    { "square",  "[]"   },
    { "diamond", "<>"   },
    { "top",     "T"    },
    { "bot",     "_|_"  },
    { "ell",     "l"    },
    { "hbar",    "h"    },
    { "Re",      "Re"   },
    { "Im",      "Im"   },
    { "aleph",   "Aleph"},

    // ─── Function names — emit upright, the user just sees "sin x" etc.
    //     (we don't currently switch font, but keeping them as explicit
    //     entries means the supported list is documented.)
    { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" },
    { "sec", "sec" }, { "csc", "csc" }, { "cot", "cot" },
    { "arcsin", "arcsin" }, { "arccos", "arccos" }, { "arctan", "arctan" },
    { "sinh", "sinh" }, { "cosh", "cosh" }, { "tanh", "tanh" },
    { "log", "log" }, { "ln", "ln" }, { "lg", "lg" }, { "exp", "exp" },
    { "lim", "lim" }, { "limsup", "limsup" }, { "liminf", "liminf" },
    { "max", "max" }, { "min", "min" }, { "sup", "sup" }, { "inf", "inf" },
    { "det", "det" }, { "dim", "dim" }, { "deg", "deg" },
    { "gcd", "gcd" }, { "mod", "mod" }, { "bmod", "mod" }, { "pmod", "mod" },
    { "arg", "arg" }, { "ker", "ker" }, { "hom", "hom" },
    { "Pr",  "Pr"  },
};
static const int NUM_SUBS = (int)(sizeof(SUBS) / sizeof(SUBS[0]));

static String substitute_command(const String &cmd) {
    for (int i = 0; i < NUM_SUBS; i++) {
        if (cmd == SUBS[i].cmd) return String(SUBS[i].out);
    }
    // Unknown / unsupported commands: fall through as plain name so the
    // user can at least see what was meant.
    return cmd;
}

// Read the contents of a {...} group as a verbatim string (no
// recursive parsing).  Used by \text{} and the \mathXX wrappers when
// we want to preserve internal whitespace literally — parse_expression
// would otherwise eat spaces between tokens.  Caller guarantees s[p]
// points at the opening '{'.  Advances p past the closing '}'.
static String read_text_arg(const String &s, int &p) {
    String out;
    int n = s.length();
    if (p >= n || s[p] != '{') return out;
    p++;
    int depth = 1;
    while (p < n && depth > 0) {
        char ch = s[p];
        if (ch == '{') { depth++; out += ch; p++; continue; }
        if (ch == '}') { depth--; if (depth == 0) { p++; break; } out += ch; p++; continue; }
        out += ch;
        p++;
    }
    return out;
}

// Helper: parse a single-token argument for accent / wrapper commands.
// Always returns a non-NULL Node (an empty Atom on EOF / mismatch) so
// callers don't need to NULL-check.
static Node *parse_required_atom(const String &s, int &p) {
    Node *a = parse_atom(s, p);
    return a ? a : new Node(Node::Atom);
}

static Node *parse_atom(const String &s, int &p) {
    skip_ws(s, p);
    int n = s.length();
    if (p >= n) return NULL;

    char c = s[p];

    // Group: { ... }
    if (c == '{') {
        p++;
        Node *e = parse_expression(s, p, '}');
        if (p < n && s[p] == '}') p++;
        return e ? e : new Node(Node::Atom);   // empty group → empty atom
    }

    // Command: \name or single-char command
    if (c == '\\') {
        p++;
        if (p >= n) return NULL;

        // ─── Single-character spacing / escape commands ────────────────────
        // \, \: \; \!  →  fixed-width gaps; sizes mirror TeX conventions.
        // \\ \{ \} \$ \% \& \# \_ → render the literal char.
        if (!isalpha(s[p])) {
            char esc = s[p];
            p++;
            if (esc == ',')  { Node *sp = new Node(Node::Space); sp->spaceEm = 0.18f; return sp; }
            if (esc == ':')  { Node *sp = new Node(Node::Space); sp->spaceEm = 0.22f; return sp; }
            if (esc == ';')  { Node *sp = new Node(Node::Space); sp->spaceEm = 0.28f; return sp; }
            if (esc == '!')  { Node *sp = new Node(Node::Space); sp->spaceEm = -0.18f; return sp; }
            if (esc == ' ')  { Node *sp = new Node(Node::Space); sp->spaceEm = 0.33f; return sp; }
            // \\ inside a formula is a line break we don't support — treat as a
            // small space so it reads naturally on one line.
            if (esc == '\\') { Node *sp = new Node(Node::Space); sp->spaceEm = 0.33f; return sp; }
            Node *atom = new Node(Node::Atom);
            atom->text = String(esc);
            return atom;
        }
        String cmd;
        while (p < n && isalpha(s[p])) { cmd += s[p]; p++; }

        if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac") {
            Node *frac = new Node(Node::Frac);
            frac->a = parse_atom(s, p);
            frac->b = parse_atom(s, p);
            if (!frac->a) frac->a = new Node(Node::Atom);
            if (!frac->b) frac->b = new Node(Node::Atom);
            return frac;
        }
        if (cmd == "sqrt") {
            // Skip optional [n] for nth root — not supported, just consume it
            skip_ws(s, p);
            if (p < n && s[p] == '[') {
                while (p < n && s[p] != ']') p++;
                if (p < n) p++;
            }
            Node *sq = new Node(Node::Sqrt);
            sq->a = parse_atom(s, p);
            if (!sq->a) sq->a = new Node(Node::Atom);
            return sq;
        }

        // ─── Accents ────────────────────────────────────────────────────
        // \hat{x}, \bar{x}, \vec{x}, \tilde{x}, \dot{x}, \ddot{x}
        // \widehat / \widetilde render the same as their normal versions
        // (they only differ by glyph stretching in TeX — close-enough on
        // an MCU).  \overline is handled as an Over node further below
        // since it takes an arbitrary-width arg.
        Node::AccentKind ak = Node::AccHat;
        bool isAcc = true;
        if      (cmd == "hat"   || cmd == "widehat")    ak = Node::AccHat;
        else if (cmd == "bar")                          ak = Node::AccBar;
        else if (cmd == "vec")                          ak = Node::AccVec;
        else if (cmd == "tilde" || cmd == "widetilde")  ak = Node::AccTilde;
        else if (cmd == "dot")                          ak = Node::AccDot;
        else if (cmd == "ddot")                         ak = Node::AccDDot;
        else                                            isAcc = false;
        if (isAcc) {
            Node *acc = new Node(Node::Accent);
            acc->accent = ak;
            acc->a = parse_required_atom(s, p);
            return acc;
        }

        // ─── Lines above / below ────────────────────────────────────────
        if (cmd == "overline" || cmd == "overrightarrow") {
            Node *o = new Node(Node::Over);
            o->overArrow = (cmd == "overrightarrow");
            o->a = parse_required_atom(s, p);
            return o;
        }
        if (cmd == "underline") {
            Node *u = new Node(Node::Under);
            u->a = parse_required_atom(s, p);
            return u;
        }

        // ─── Boxed ──────────────────────────────────────────────────────
        if (cmd == "boxed") {
            Node *bx = new Node(Node::Box);
            bx->a = parse_required_atom(s, p);
            return bx;
        }

        // ─── Text / font wrappers — transparent ────────────────────────
        // We don't ship the font variants on this hardware so all of
        // these just render their argument in the regular Montserrat
        // face.  For \text{} we preserve internal whitespace verbatim
        // (otherwise the parser eats spaces and "Hello world" collapses).
        if (cmd == "text" || cmd == "textrm" || cmd == "textbf" ||
            cmd == "textit" || cmd == "texttt" || cmd == "textsf") {
            skip_ws(s, p);
            if (p < n && s[p] == '{') {
                String txt = read_text_arg(s, p);
                Node *atom = new Node(Node::Atom);
                atom->text = txt;
                return atom;
            }
            return new Node(Node::Atom);
        }
        if (cmd == "mathrm"  || cmd == "mathbf" || cmd == "mathit" ||
            cmd == "mathsf"  || cmd == "mathtt" || cmd == "mathbb" ||
            cmd == "mathcal" || cmd == "mathfrak" || cmd == "mathscr" ||
            cmd == "operatorname") {
            // Argument is a normal sub-expression (not raw text).
            return parse_required_atom(s, p);
        }

        // ─── Delimiter-sizing markers: just drop the keyword ───────────
        // \left( ... \right) → consume the marker, the next char is the
        // actual delimiter the user wanted to render.  Same for \big( etc.
        if (cmd == "left" || cmd == "right" ||
            cmd == "big"  || cmd == "Big"  || cmd == "bigg" || cmd == "Bigg" ||
            cmd == "bigl" || cmd == "Bigl" || cmd == "biggl" || cmd == "Biggl" ||
            cmd == "bigr" || cmd == "Bigr" || cmd == "biggr" || cmd == "Biggr") {
            // Don't skip whitespace — TeX requires the delim to be next.
            if (p < n) {
                char d = s[p];
                // \left.   /  \right. → invisible delimiter; emit nothing.
                if (d == '.') { p++; return new Node(Node::Atom); }
                p++;
                Node *atom = new Node(Node::Atom);
                atom->text = String(d);
                return atom;
            }
            return new Node(Node::Atom);
        }

        // ─── Multi-character spacing words ─────────────────────────────
        if (cmd == "quad")  { Node *sp = new Node(Node::Space); sp->spaceEm = 1.0f; return sp; }
        if (cmd == "qquad") { Node *sp = new Node(Node::Space); sp->spaceEm = 2.0f; return sp; }
        if (cmd == "space") { Node *sp = new Node(Node::Space); sp->spaceEm = 0.33f; return sp; }
        if (cmd == "thinspace")   { Node *sp = new Node(Node::Space); sp->spaceEm = 0.18f; return sp; }
        if (cmd == "medspace")    { Node *sp = new Node(Node::Space); sp->spaceEm = 0.22f; return sp; }
        if (cmd == "thickspace")  { Node *sp = new Node(Node::Space); sp->spaceEm = 0.28f; return sp; }
        if (cmd == "negthinspace"){ Node *sp = new Node(Node::Space); sp->spaceEm = -0.18f; return sp; }

        // ─── Symbol substitution ───────────────────────────────────────
        Node *atom = new Node(Node::Atom);
        atom->text = substitute_command(cmd);
        return atom;
    }

    // Stop tokens — caller handles
    if (c == '}' || c == '^' || c == '_') return NULL;

    // Ordinary single character.  Group consecutive digits / letters into
    // one atom for cleaner rendering kerning (one label widget instead of
    // many).  Stop at any structural char.
    String text;
    while (p < n) {
        char d = s[p];
        if (d == '\\' || d == '{' || d == '}' || d == '^' || d == '_') break;
        if (d == ' ' || d == '\t' || d == '\n' || d == '\r') break;
        text += d;
        p++;
    }
    Node *atom = new Node(Node::Atom);
    atom->text = text;
    return atom;
}

static Node *parse_expression(const String &s, int &p, char until) {
    int n = s.length();
    std::vector<Node *> children;

    while (p < n) {
        skip_ws(s, p);
        if (p >= n) break;
        if (s[p] == until) break;
        if (s[p] == '}') break;

        Node *atom = parse_atom(s, p);
        if (!atom) break;

        // Attach super/subscripts to the most recent atom.  Multiple
        // ^ / _ allowed in any order (e.g. "x_i^2" same as "x^2_i").
        while (p < n && (s[p] == '^' || s[p] == '_')) {
            char op = s[p];
            p++;
            Node *exp = parse_atom(s, p);
            if (!exp) exp = new Node(Node::Atom);

            Node *ss;
            if (atom->kind == Node::SupSub) {
                ss = atom;
            } else {
                ss = new Node(Node::SupSub);
                ss->a = atom;
            }
            if (op == '^') {
                if (ss->b) delete ss->b;
                ss->b = exp;
            } else {
                if (ss->c) delete ss->c;
                ss->c = exp;
            }
            atom = ss;
        }

        children.push_back(atom);
    }

    if (children.empty()) return new Node(Node::Atom);
    if (children.size() == 1) return children[0];

    Node *row = new Node(Node::Row);
    row->row = children;
    return row;
}

// ─── Layout ─────────────────────────────────────────────────────────────────
//
// Each node's bounding box is computed bottom-up.  Atoms use font metrics.
// Composite nodes combine children with TeX-ish but simplified spacing.
//
// "baseline" is the distance from the top of the bbox to the imaginary line
// that text sits on — used to vertically align siblings in a Row so that
// (e.g.) "1 + frac" puts the "1" at the visual middle of the fraction
// instead of at the top.

// Spacing scales with the main font's line height so layout looks balanced
// at every font size (a 2 px fraction bar that looks fine under font 14
// looks like dental floss under font 48).  Helpers below compute each
// metric from a font; they're pure functions of line_height so they're
// cheap and consistent.
static int frac_gap   (const lv_font_t *f) { int h = lv_font_get_line_height(f); return h / 8 + 1; }
static int frac_bar_t (const lv_font_t *f) { int t = lv_font_get_line_height(f) / 18; return t < 2 ? 2 : t; }
static int frac_pad_x (const lv_font_t *f) { return lv_font_get_line_height(f) / 8 + 2; }
static int sqrt_gap   (const lv_font_t *f) { int h = lv_font_get_line_height(f); return h / 10 + 1; }
static int sqrt_bar_t (const lv_font_t *f) { return frac_bar_t(f); }
static int sqrt_tick_w(const lv_font_t *f) { return lv_font_get_line_height(f) / 4 + 2; }
static int sqrt_pad_r (const lv_font_t *f) { return lv_font_get_line_height(f) / 5; }
static int sup_raise  (const lv_font_t *f) { return lv_font_get_line_height(f) / 5; }
static int sub_drop   (const lv_font_t *f) { return lv_font_get_line_height(f) / 7; }
static int ss_gap     (const lv_font_t *f) { (void)f; return 1; }

static void layout(Node *n, const FontPair &fp);

static int font_ascent(const lv_font_t *f) {
    // Approximation: ascent ≈ line_height * 0.78.  LVGL doesn't expose
    // ascent/descent directly on lv_font_t in v8 without diving into
    // the internal struct, so we use a fixed ratio that looks right for
    // the Montserrat family.
    return (int)(lv_font_get_line_height(f) * 0.78f);
}

static void layout_atom(Node *n, const FontPair &fp) {
    lv_point_t sz;
    lv_txt_get_size(&sz, n->text.c_str(), fp.main, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    // Empty atoms (used as placeholders) still take a tiny slot so super/sub
    // anchoring math doesn't explode.
    n->w = sz.x > 0 ? sz.x : 1;
    n->h = sz.y > 0 ? sz.y : lv_font_get_line_height(fp.main);
    n->baseline = font_ascent(fp.main);
}

static void layout_row(Node *n, const FontPair &fp) {
    int total_w = 0;
    int max_above = 0;   // tallest distance from baseline up
    int max_below = 0;   // tallest distance from baseline down
    for (auto *c : n->row) {
        layout(c, fp);
        total_w += c->w;
        int above = c->baseline;
        int below = c->h - c->baseline;
        if (above > max_above) max_above = above;
        if (below > max_below) max_below = below;
    }
    n->w = total_w;
    n->h = max_above + max_below;
    n->baseline = max_above;
}

static void layout_frac(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    layout(n->b, fp);
    int gap   = frac_gap(fp.main);
    int bar_t = frac_bar_t(fp.main);
    int pad_x = frac_pad_x(fp.main);
    int bar_w = (n->a->w > n->b->w ? n->a->w : n->b->w) + 2 * pad_x;
    n->w = bar_w;
    n->h = n->a->h + gap + bar_t + gap + n->b->h;
    // Visually, the fraction bar acts as the baseline so adjacent text
    // centers around it.
    n->baseline = n->a->h + gap + bar_t / 2;
}

static void layout_sqrt(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int gap   = sqrt_gap(fp.main);
    int bar_t = sqrt_bar_t(fp.main);
    int tick  = sqrt_tick_w(fp.main);
    int pad_r = sqrt_pad_r(fp.main);
    n->w = tick + n->a->w + pad_r;
    n->h = gap + bar_t + n->a->h;
    n->baseline = gap + bar_t + n->a->baseline;
}

static void layout_supsub(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int raise = sup_raise(fp.main);
    int drop  = sub_drop(fp.main);
    int gap   = ss_gap(fp.main);

    int sup_w = 0, sup_h = 0;
    int sub_w = 0, sub_h = 0;
    if (n->b) {
        FontPair sfp = { fp.small, fp.small };
        layout(n->b, sfp);
        sup_w = n->b->w + gap;
        sup_h = n->b->h;
    }
    if (n->c) {
        FontPair sfp = { fp.small, fp.small };
        layout(n->c, sfp);
        sub_w = n->c->w + gap;
        sub_h = n->c->h;
    }
    int side_w = sup_w > sub_w ? sup_w : sub_w;
    n->w = n->a->w + side_w;

    // Vertical extent: super reaches above the base by (sup_h - raise),
    // sub reaches below the base by (sub_h - drop); clamp at 0 so a small
    // super that fits inside the base height doesn't add anything.
    int above = (n->b && sup_h > raise) ? (sup_h - raise) : 0;
    int below = (n->c && sub_h > drop)  ? (sub_h - drop)  : 0;
    n->h = above + n->a->h + below;
    n->baseline = above + n->a->baseline;
}

// ─── Accent / over / under / box / space layout ────────────────────────────

// Vertical gap between body and accent / overline / underline.  Tuned to
// match the existing fraction-bar feel — small enough that the accent
// reads as part of the symbol, not a separate shape.
static int accent_gap(const lv_font_t *f) { return lv_font_get_line_height(f) / 14 + 1; }
static int accent_h  (const lv_font_t *f) { return lv_font_get_line_height(f) / 5  + 1; }
static int box_pad   (const lv_font_t *f) { return lv_font_get_line_height(f) / 10 + 2; }

static void layout_accent(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int extra = accent_gap(fp.main) + accent_h(fp.main);
    n->w = n->a->w;
    n->h = n->a->h + extra;
    n->baseline = n->a->baseline + extra;   // body shifted down by `extra`
}

static void layout_over(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int gap = accent_gap(fp.main);
    int barT = frac_bar_t(fp.main);
    n->w = n->a->w;
    n->h = n->a->h + gap + barT;
    n->baseline = n->a->baseline + gap + barT;
}

static void layout_under(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int gap = accent_gap(fp.main);
    int barT = frac_bar_t(fp.main);
    n->w = n->a->w;
    n->h = n->a->h + gap + barT;
    n->baseline = n->a->baseline;   // bar lives below baseline
}

static void layout_box(Node *n, const FontPair &fp) {
    layout(n->a, fp);
    int pad = box_pad(fp.main);
    n->w = n->a->w + 2 * pad;
    n->h = n->a->h + 2 * pad;
    n->baseline = n->a->baseline + pad;
}

static void layout_space(Node *n, const FontPair &fp) {
    int em = lv_font_get_line_height(fp.main);
    int w  = (int)(n->spaceEm * em + 0.5f);
    // Negative spaces (\!) shouldn't actually overlap text — clamp to 0
    // for layout but the renderer can still apply the negative offset.
    if (w < 0) w = 0;
    n->w = w;
    n->h = em;
    n->baseline = font_ascent(fp.main);
}

static void layout(Node *n, const FontPair &fp) {
    switch (n->kind) {
        case Node::Atom:   layout_atom(n, fp);   break;
        case Node::Row:    layout_row(n, fp);    break;
        case Node::Frac:   layout_frac(n, fp);   break;
        case Node::Sqrt:   layout_sqrt(n, fp);   break;
        case Node::SupSub: layout_supsub(n, fp); break;
        case Node::Accent: layout_accent(n, fp); break;
        case Node::Over:   layout_over(n, fp);   break;
        case Node::Under:  layout_under(n, fp);  break;
        case Node::Box:    layout_box(n, fp);    break;
        case Node::Space:  layout_space(n, fp);  break;
    }
}

// ─── Render ─────────────────────────────────────────────────────────────────
//
// All rendered widgets become children of `parent`.  Coordinates are
// absolute within `parent`.  We use:
//   - lv_label for text atoms
//   - lv_obj rectangles for fraction bars and sqrt overlines (cheap, crisp)
//   - lv_line for the diagonal radical "check mark"  (point arrays kept
//     alive via lv_obj_set_user_data + a delete cb that frees them)

static lv_color_t col_fg() { return lv_color_white(); }

static void free_user_data_cb(lv_event_t *e) {
    void *p = lv_obj_get_user_data(lv_event_get_target(e));
    if (p) free(p);
}

static void render(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp);

static void render_atom(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    if (n->text.length() == 0) return;

    // Special case: \pm — render as overlaid + and − so we don't depend on
    // U+00B1 being in the font.  Substitute_command emits "+/-" as a marker.
    if (n->text == "+/-" || n->text == "-/+") {
        // Just fall through to plain text; "+/-" reads cleanly without
        // requiring the actual ± glyph.  In v2 we could draw a real ±
        // by overlaying a "+" and "−" with custom positioning.
    }

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, n->text.c_str());
    lv_obj_set_style_text_font(lbl, fp.main, 0);
    lv_obj_set_style_text_color(lbl, col_fg(), 0);
    lv_obj_set_pos(lbl, x, y);
}

static void render_row(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int cx = x;
    for (auto *c : n->row) {
        // Baseline-align: shift child down so its baseline matches row's
        int childY = y + (n->baseline - c->baseline);
        render(c, parent, cx, childY, fp);
        cx += c->w;
    }
}

static void render_hline(lv_obj_t *parent, int x, int y, int w, int thickness) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, w, thickness);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, col_fg(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 1, 0);
}

static void render_frac(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int gap   = frac_gap(fp.main);
    int bar_t = frac_bar_t(fp.main);
    int pad_x = frac_pad_x(fp.main);
    // Numerator centered above the bar
    int numX = x + (n->w - n->a->w) / 2;
    render(n->a, parent, numX, y, fp);
    render_hline(parent, x + pad_x, y + n->a->h + gap,
                 n->w - 2 * pad_x, bar_t);
    int denX = x + (n->w - n->b->w) / 2;
    render(n->b, parent, denX, y + n->a->h + gap + bar_t + gap, fp);
}

static void render_sqrt(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int gap   = sqrt_gap(fp.main);
    int bar_t = sqrt_bar_t(fp.main);
    int tick  = sqrt_tick_w(fp.main);
    int pad_r = sqrt_pad_r(fp.main);
    // Stroke thickness scales with font height so the radical reads at any size
    int stroke = bar_t;
    // Overline above the argument
    int barX = x + tick;
    int barY = y;
    int barW = n->a->w + pad_r;
    render_hline(parent, barX, barY, barW, bar_t);

    // Diagonal radical "check mark" — three points: low-left → low-mid →
    // top-of-stroke (where it meets the overline).  lv_line owns its own
    // points array via user_data so we can free it on widget delete.
    lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * 3);
    if (pts) {
        pts[0].x = x;
        pts[0].y = y + (n->h * 2 / 3);
        pts[1].x = x + tick / 2;
        pts[1].y = y + n->h - 1;
        pts[2].x = x + tick;
        pts[2].y = y + bar_t / 2;

        lv_obj_t *line = lv_line_create(parent);
        lv_line_set_points(line, pts, 3);
        lv_obj_set_style_line_color(line, col_fg(), 0);
        lv_obj_set_style_line_width(line, stroke, 0);
        lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_user_data(line, pts);
        lv_obj_add_event_cb(line, free_user_data_cb, LV_EVENT_DELETE, NULL);
    }

    render(n->a, parent, x + tick, y + gap + bar_t, fp);
}

static void render_supsub(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int raise = sup_raise(fp.main);
    int drop  = sub_drop(fp.main);
    int gap   = ss_gap(fp.main);

    // Match the layout_supsub() math exactly so siblings line up correctly.
    int sup_h = n->b ? n->b->h : 0;
    int above = (n->b && sup_h > raise) ? (sup_h - raise) : 0;

    // Base sits below the super's overhang
    render(n->a, parent, x, y + above, fp);

    int rightX = x + n->a->w + gap;
    if (n->b) {
        FontPair sfp = { fp.small, fp.small };
        render(n->b, parent, rightX, y, sfp);
    }
    if (n->c) {
        int subY = y + above + n->a->h - drop;
        FontPair sfp = { fp.small, fp.small };
        render(n->c, parent, rightX, subY, sfp);
    }
}

// ─── Accent / over / under / box / space rendering ─────────────────────────

// Tiny single-char label helper — used for accent glyphs (^, ~, ., ¨, →).
static void render_text_at(lv_obj_t *parent, int x, int y, const char *txt,
                            const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, col_fg(), 0);
    lv_obj_set_pos(lbl, x, y);
}

// Draw a 1px-style hollow rectangle by stacking four hlines.  Cheaper
// than enabling LV_DRAW_COMPLEX borders on this MCU.
static void render_box_outline(lv_obj_t *parent, int x, int y, int w, int h, int t) {
    render_hline(parent, x,         y,         w, t);   // top
    render_hline(parent, x,         y + h - t, w, t);   // bottom
    render_hline(parent, x,         y,         t, h);   // left
    render_hline(parent, x + w - t, y,         t, h);   // right
}

static void render_accent(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int gap   = accent_gap(fp.main);
    int accH  = accent_h(fp.main);
    int bodyY = y + gap + accH;
    render(n->a, parent, x, bodyY, fp);

    int accY = y;
    int cx   = x + n->a->w / 2;
    switch (n->accent) {
        case Node::AccHat: {
            // Two short diagonal strokes meeting at apex.
            int half = n->a->w / 2;
            if (half < 4) half = 4;
            int t = frac_bar_t(fp.main);
            // Left stroke
            lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
            if (pts) {
                pts[0].x = cx - half / 2;  pts[0].y = accY + accH - 1;
                pts[1].x = cx;             pts[1].y = accY + 1;
                lv_obj_t *L = lv_line_create(parent);
                lv_line_set_points(L, pts, 2);
                lv_obj_set_style_line_color(L, col_fg(), 0);
                lv_obj_set_style_line_width(L, t, 0);
                lv_obj_set_style_line_opa(L, LV_OPA_COVER, 0);
                lv_obj_set_user_data(L, pts);
                lv_obj_add_event_cb(L, free_user_data_cb, LV_EVENT_DELETE, NULL);
            }
            lv_point_t *pts2 = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
            if (pts2) {
                pts2[0].x = cx;            pts2[0].y = accY + 1;
                pts2[1].x = cx + half / 2; pts2[1].y = accY + accH - 1;
                lv_obj_t *R = lv_line_create(parent);
                lv_line_set_points(R, pts2, 2);
                lv_obj_set_style_line_color(R, col_fg(), 0);
                lv_obj_set_style_line_width(R, t, 0);
                lv_obj_set_style_line_opa(R, LV_OPA_COVER, 0);
                lv_obj_set_user_data(R, pts2);
                lv_obj_add_event_cb(R, free_user_data_cb, LV_EVENT_DELETE, NULL);
            }
            break;
        }
        case Node::AccBar: {
            int t = frac_bar_t(fp.main);
            render_hline(parent, x, accY + accH - t, n->a->w, t);
            break;
        }
        case Node::AccVec: {
            // Short horizontal bar with a small arrowhead on the right.
            int t = frac_bar_t(fp.main);
            int barW = n->a->w;
            int barY = accY + accH - t;
            render_hline(parent, x, barY, barW, t);
            int head = accH;
            if (head > 6) head = 6;
            // Diagonal arrowhead lines.
            lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
            if (pts) {
                pts[0].x = x + barW;        pts[0].y = barY + t / 2;
                pts[1].x = x + barW - head; pts[1].y = barY + t / 2 - head;
                lv_obj_t *L = lv_line_create(parent);
                lv_line_set_points(L, pts, 2);
                lv_obj_set_style_line_color(L, col_fg(), 0);
                lv_obj_set_style_line_width(L, t, 0);
                lv_obj_set_style_line_opa(L, LV_OPA_COVER, 0);
                lv_obj_set_user_data(L, pts);
                lv_obj_add_event_cb(L, free_user_data_cb, LV_EVENT_DELETE, NULL);
            }
            lv_point_t *pts2 = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
            if (pts2) {
                pts2[0].x = x + barW;        pts2[0].y = barY + t / 2;
                pts2[1].x = x + barW - head; pts2[1].y = barY + t / 2 + head;
                lv_obj_t *R = lv_line_create(parent);
                lv_line_set_points(R, pts2, 2);
                lv_obj_set_style_line_color(R, col_fg(), 0);
                lv_obj_set_style_line_width(R, t, 0);
                lv_obj_set_style_line_opa(R, LV_OPA_COVER, 0);
                lv_obj_set_user_data(R, pts2);
                lv_obj_add_event_cb(R, free_user_data_cb, LV_EVENT_DELETE, NULL);
            }
            break;
        }
        case Node::AccTilde: {
            // Render "~" as a small label centered above the body.
            render_text_at(parent, cx - 4, accY - 2, "~", fp.small);
            break;
        }
        case Node::AccDot: {
            // Single dot — small filled square is plenty at MCU scale.
            int t = frac_bar_t(fp.main) + 1;
            lv_obj_t *d = lv_obj_create(parent);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, t, t);
            lv_obj_set_pos(d, cx - t / 2, accY + accH / 2 - t / 2);
            lv_obj_set_style_radius(d, t, 0);
            lv_obj_set_style_bg_color(d, col_fg(), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            break;
        }
        case Node::AccDDot: {
            int t = frac_bar_t(fp.main) + 1;
            int sep = t + 2;
            for (int i = 0; i < 2; i++) {
                int dx = (i == 0) ? -sep / 2 : sep / 2;
                lv_obj_t *d = lv_obj_create(parent);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, t, t);
                lv_obj_set_pos(d, cx + dx - t / 2, accY + accH / 2 - t / 2);
                lv_obj_set_style_radius(d, t, 0);
                lv_obj_set_style_bg_color(d, col_fg(), 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            }
            break;
        }
    }
}

static void render_over(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int gap  = accent_gap(fp.main);
    int barT = frac_bar_t(fp.main);
    render_hline(parent, x, y, n->a->w, barT);
    if (n->overArrow) {
        // Tiny arrowhead on the right end of the bar.
        int head = lv_font_get_line_height(fp.main) / 6;
        if (head > 6) head = 6;
        if (head < 3) head = 3;
        lv_point_t *pts = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
        if (pts) {
            pts[0].x = x + n->a->w;        pts[0].y = y + barT / 2;
            pts[1].x = x + n->a->w - head; pts[1].y = y + barT / 2 - head;
            lv_obj_t *L = lv_line_create(parent);
            lv_line_set_points(L, pts, 2);
            lv_obj_set_style_line_color(L, col_fg(), 0);
            lv_obj_set_style_line_width(L, barT, 0);
            lv_obj_set_style_line_opa(L, LV_OPA_COVER, 0);
            lv_obj_set_user_data(L, pts);
            lv_obj_add_event_cb(L, free_user_data_cb, LV_EVENT_DELETE, NULL);
        }
        lv_point_t *pts2 = (lv_point_t *)malloc(sizeof(lv_point_t) * 2);
        if (pts2) {
            pts2[0].x = x + n->a->w;        pts2[0].y = y + barT / 2;
            pts2[1].x = x + n->a->w - head; pts2[1].y = y + barT / 2 + head;
            lv_obj_t *R = lv_line_create(parent);
            lv_line_set_points(R, pts2, 2);
            lv_obj_set_style_line_color(R, col_fg(), 0);
            lv_obj_set_style_line_width(R, barT, 0);
            lv_obj_set_style_line_opa(R, LV_OPA_COVER, 0);
            lv_obj_set_user_data(R, pts2);
            lv_obj_add_event_cb(R, free_user_data_cb, LV_EVENT_DELETE, NULL);
        }
    }
    render(n->a, parent, x, y + gap + barT, fp);
}

static void render_under(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int gap  = accent_gap(fp.main);
    int barT = frac_bar_t(fp.main);
    render(n->a, parent, x, y, fp);
    render_hline(parent, x, y + n->a->h + gap, n->a->w, barT);
}

static void render_box(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    int pad  = box_pad(fp.main);
    int barT = frac_bar_t(fp.main);
    render_box_outline(parent, x, y, n->w, n->h, barT);
    render(n->a, parent, x + pad, y + pad, fp);
}

// Space nodes are pure horizontal padding — nothing to draw.
static void render_space(Node *, lv_obj_t *, int, int, const FontPair &) {}

static void render(Node *n, lv_obj_t *parent, int x, int y, const FontPair &fp) {
    switch (n->kind) {
        case Node::Atom:   render_atom(n, parent, x, y, fp);   break;
        case Node::Row:    render_row(n, parent, x, y, fp);    break;
        case Node::Frac:   render_frac(n, parent, x, y, fp);   break;
        case Node::Sqrt:   render_sqrt(n, parent, x, y, fp);   break;
        case Node::SupSub: render_supsub(n, parent, x, y, fp); break;
        case Node::Accent: render_accent(n, parent, x, y, fp); break;
        case Node::Over:   render_over(n, parent, x, y, fp);   break;
        case Node::Under:  render_under(n, parent, x, y, fp);  break;
        case Node::Box:    render_box(n, parent, x, y, fp);    break;
        case Node::Space:  render_space(n, parent, x, y, fp);  break;
    }
}

// ─── Public entry ───────────────────────────────────────────────────────────

lv_obj_t *latex_render(lv_obj_t *parent, const char *latex,
                       lv_coord_t max_w, lv_coord_t max_h,
                       bool fit_circle) {
    if (!latex || !*latex) return NULL;

    String src = latex;

    // Parse once — the tree is small and parsing is cheap
    int p = 0;
    Node *root = parse_expression(src, p, '\0');
    if (!root) return NULL;

    // For a circle of diameter D, any axis-aligned rect (w, h) centered in
    // it fits iff w² + h² ≤ D² (corners touch the circle when equal).
    const long diam_sq = (long)max_w * (long)max_w;

    int chosenPair = 0;
    for (int i = 0; i < NUM_FONT_PAIRS; i++) {
        layout(root, FONT_PAIRS[i]);
        bool fits;
        if (fit_circle) {
            long s = (long)root->w * (long)root->w +
                     (long)root->h * (long)root->h;
            fits = (max_w == 0) || (s <= diam_sq);
        } else {
            fits = (max_w == 0 || root->w <= max_w) &&
                   (max_h == 0 || root->h <= max_h);
        }
        chosenPair = i;
        if (fits) break;
    }

    LOG2("[latex] rendered \"%s\" → %dx%d using font pair %d (%s fit)\n",
         latex, root->w, root->h, chosenPair,
         fit_circle ? "circle" : "rect");

    // Container sized to the expression's bbox
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, root->w, root->h);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cont, 0, 0);

    render(root, cont, 0, 0, FONT_PAIRS[chosenPair]);

    delete root;
    return cont;
}
