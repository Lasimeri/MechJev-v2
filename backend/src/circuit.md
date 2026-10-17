# circuit.c

A differentiable logic gate network: Petersen, Borgelt, Kuehne, Deussen,
"Deep Differentiable Logic Gate Networks", NeurIPS 2022, arXiv:2210.08277
(read 2026-10-17 through Exa).

- **Neurons.** Each has two inputs, wired once from a seed and fixed. The
  wiring is never stored; it is recomputed from the seed.
- **Gates.** A neuron learns which of the 16 two-input Boolean functions it
  is, as a softmax over 16 weights drawn from N(0,1) at the start (the
  paper's initialization).
- **Training.** The relaxed network computes each gate as the expected
  value under independent inputs (Table 1, the probabilistic T-norm),
  with the weighted average over gates. The gradients are exact: a gate's
  weight gets `p_i (f_i - y)`, and each input gets the gate's partial
  derivatives. The optimizer is Adam at a constant 0.01, the paper's.
- **Inference.** Each neuron takes its most likely gate and runs on bits,
  with no multiply anywhere.
- **Export.** `circuit_export` writes that discrete circuit as a C
  function (Aster's "compiled directly into C").

Measured (`backend/tests/learner_test.c`):
- the 16 gates on bits equal their truth tables;
- the exported C, compiled with tcc, is identical to the interpreter on
  200 inputs;
- on the grounding toy, the relaxed and discrete circuits pick the same
  word on 1.000 of the held-out inputs (the paper reports a gap under 0.1%
  on MNIST).
