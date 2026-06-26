---
title: Math rendering
tags: [demo, math]
---

# LaTeX math

Descry renders inline `$…$` and display `$$…$$` LaTeX-math spans
typographically — Greek letters, operators, and simple super/subscripts
become real Unicode glyphs. It is a typographic pass, not a full TeX engine.

## Inline

Euler's identity $e^{i\pi} + 1 = 0$ is famous. A bound like
$\alpha \leq \beta \times \gamma$ and a set test $x \in \mathbb{R}$ read
cleanly inline.

The quadratic roots are $x = -b \pm \sqrt{b^2 - 4ac}$ over $2a$.

## Display

$$ \sum_{i=1}^{n} i = \frac{n(n+1)}{2} $$

$$ \int_{0}^{\infty} e^{-x^2} \, dx = \frac{\sqrt{\pi}}{2} $$

## Mixed prose

When $n \rightarrow \infty$ the error $\epsilon \to 0$, so
$f(x) \approx g(x)$ for all $x \geq x_0$. Note that a literal price like
$5 or $10 (no closing pair around a non-math run) stays literal.
