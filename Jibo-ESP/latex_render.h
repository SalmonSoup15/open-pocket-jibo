#pragma once
#include <lvgl.h>

// Render a meaningful subset of LaTeX into an LVGL container.
//
// SUPPORTED:
//   - ASCII letters / digits / common punctuation
//   - \frac{a}{b}                       — proper fraction with bar
//   - \sqrt{x}                          — radical sign + overline
//   - x^{...}, x^c, x_{...}, x_i        — super/subscripts (one each)
//   - Greek letters \alpha .. \omega and \Alpha .. \Omega (spelled-out
//     ASCII fallbacks since stock Montserrat has no Greek glyphs —
//     "α" emerges as "α"-the-word "alpha", which is unambiguous)
//   - Operators: \sum \prod \int \oint \partial \nabla
//   - Relations: \leq \geq \neq \approx \equiv \sim \cong \propto \perp
//                \parallel \ll \gg \prec \succ
//   - Set theory: \in \notin \subset \supset \subseteq \supseteq \cup
//                 \cap \setminus \emptyset \varnothing
//   - Logic:     \forall \exists \nexists \neg \land \lor \implies \iff
//   - Arrows:    \to \gets \rightarrow \leftarrow \leftrightarrow
//                \Rightarrow \Leftarrow \Leftrightarrow \mapsto
//   - Functions (rendered upright): \sin \cos \tan \sec \csc \cot
//                \arcsin \arccos \arctan \sinh \cosh \tanh
//                \log \ln \lg \exp \lim \limsup \liminf \max \min
//                \sup \inf \det \dim \deg \gcd \mod \arg \ker
//   - Misc:      \pm \mp \cdot \times \div \ast \star \bullet \circ
//                \dagger \ddagger \infty \angle \prime \degree
//                \ldots \cdots \vdots \ddots
//   - Accents:   \hat{x} \bar{x} \vec{x} \tilde{x} \dot{x} \ddot{x}
//   - Lines:     \overline{...} \underline{...} \overrightarrow{...}
//   - Boxed:     \boxed{...}                 — frame around content
//   - Text:      \text{...} \mathrm{...} \mathbf{...} \mathit{...}
//                \mathsf{...} \mathtt{...} \mathbb{...} \mathcal{...}
//                — all transparent (no font swap; we don't ship the
//                  font variants on this hardware)
//   - Delim:     \left( \right) \big( \Big( \bigg( \Bigg(  — markers
//                consumed; following char emitted as the literal delimiter
//   - Spacing:   \, \: \; \! \quad \qquad \space   — explicit space atoms
//   - Unknown commands still fall through as their plain name so the user
//     at least sees *what* was meant.
//
// NOT SUPPORTED:
//   - matrices, alignments, multi-line equations
//   - n-th roots (\sqrt[n]{x})  — the [n] is silently consumed
//   - true font swap for \mathbb / \mathbf etc. (uses Montserrat throughout)
//
// The returned container has been sized to fit the rendered expression and
// positioned at (0, 0) of `parent`.  Caller is responsible for centering it
// (lv_obj_align) and eventually deleting it.  All child widgets are owned
// by the container and freed automatically on lv_obj_del.
//
// `max_w` and `max_h` are advisory — if the natural rendered size exceeds
// either, the renderer will retry with a smaller font pair.  Pass 0 for
// unbounded.  Returns NULL on parse failure or alloc failure.
//
// `fit_circle`: when true, max_w is interpreted as the diameter of a
// circular fit region and the size check uses w² + h² ≤ max_w² instead
// of w ≤ max_w && h ≤ max_h.  Use this for round displays so wide
// formulas (e.g. the quadratic) can extend toward the corners of the
// circle without being unnecessarily down-sized to fit an inscribed
// square.
lv_obj_t *latex_render(lv_obj_t *parent, const char *latex,
                       lv_coord_t max_w, lv_coord_t max_h,
                       bool fit_circle = false);
