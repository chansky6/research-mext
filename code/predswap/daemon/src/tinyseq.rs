//! M4: a two-layer, reset-after GRU over delta tokens. Model storage is
//! 14,080 f32s; the activation tape and gradients exist only during training.

const K: usize = 64;
const E: usize = 16;
const H: usize = 32;
const T: usize = 32;
const RATE: f32 = 0.1;
type Hidden = [f32; H];

#[inline]
fn has_sve() -> bool {
    #[cfg(all(target_arch = "aarch64", target_os = "linux"))]
    { return std::arch::is_aarch64_feature_detected!("sve"); }
    #[cfg(not(all(target_arch = "aarch64", target_os = "linux")))]
    { false }
}

#[inline]
fn dot<const N: usize, const SVE: bool>(a: &[f32; N], b: &[f32; N]) -> f32 {
    #[cfg(all(target_arch = "aarch64", target_os = "linux"))]
    if SVE {
        // The only callers selecting this specialization check Linux HWCAP.
        // whilelo masks both loads, including on machines with VL > N.
        let sum: f32;
        unsafe {
            std::arch::asm!(
                ".arch_extension sve",
                "mov {i}, xzr", "dup z0.s, #0",
                "2:", "whilelo p0.s, {i}, {n}",
                "ld1w {{z1.s}}, p0/z, [{a}, {i}, lsl #2]",
                "ld1w {{z2.s}}, p0/z, [{b}, {i}, lsl #2]",
                "fmla z0.s, p0/m, z1.s, z2.s", "incw {i}",
                "cmp {i}, {n}", "b.lo 2b",
                "ptrue p0.s", "faddv s0, p0, z0.s",
                i = out(reg) _, n = in(reg) N, a = in(reg) a.as_ptr(), b = in(reg) b.as_ptr(),
                lateout("s0") sum, out("v1") _, out("v2") _, out("p0") _,
                options(nostack, readonly),
            );
        }
        return sum;
    }
    a.iter().zip(b).map(|(u, v)| u * v).sum()
}

#[inline]
fn axpy<const N: usize>(dst: &mut [f32; N], alpha: f32, x: &[f32; N]) {
    for (d, s) in dst.iter_mut().zip(x) { *d += alpha * s; }
}

#[derive(Clone)]
struct Layer<const I: usize> {
    // Row blocks: update, reset, candidate.
    w: [[f32; I]; 3 * H],
    u: [Hidden; 3 * H],
    b: [f32; 3 * H],
}

#[derive(Clone, Copy)]
struct Step {
    h: Hidden, z: Hidden, r: Hidden, n: Hidden, un: Hidden,
}
const START: Step = Step { h: [0.; H], z: [0.; H], r: [0.; H], n: [0.; H], un: [0.; H] };

impl<const I: usize> Layer<I> {
    const ZERO: Self = Self { w: [[0.; I]; 3*H], u: [[0.; H]; 3*H], b: [0.; 3*H] };

    fn step<const S: bool>(&self, x: &[f32; I], prev: &Hidden) -> Step {
        let gate = |j: usize| {
            let v = self.b[j] + dot::<I,S>(&self.w[j], x) + dot::<H,S>(&self.u[j], prev);
            1. / (1. + (-v).exp())
        };
        let mut s = START;
        for j in 0..H {
            s.z[j] = gate(j);
            s.r[j] = gate(H+j);
            s.un[j] = dot::<H,S>(&self.u[2*H+j], prev);
            s.n[j] = (self.b[2*H+j] + dot::<I,S>(&self.w[2*H+j], x) + s.r[j]*s.un[j]).tanh();
            s.h[j] = s.z[j]*prev[j] + (1.-s.z[j])*s.n[j];
        }
        s
    }

    /// Accumulate parameter gradients and return gradients for both inputs.
    fn backward(&self, g: &mut Self, s: &Step, x: &[f32; I], prev: &Hidden, dh: &Hidden)
        -> ([f32; I], Hidden)
    {
        let mut dx = [0.; I];
        let mut dp = [0.; H];
        for j in 0..H {
            let dz = dh[j]*(prev[j]-s.n[j])*s.z[j]*(1.-s.z[j]);
            let dn = dh[j]*(1.-s.z[j])*(1.-s.n[j]*s.n[j]);
            let dr = dn*s.un[j]*s.r[j]*(1.-s.r[j]);
            dp[j] += dh[j]*s.z[j];
            for (row, dw, du) in [(j,dz,dz), (H+j,dr,dr), (2*H+j,dn,dn*s.r[j])] {
                g.b[row] += dw;
                axpy(&mut g.w[row], dw, x);
                axpy(&mut g.u[row], du, prev);
                axpy(&mut dx, dw, &self.w[row]);
                axpy(&mut dp, du, &self.u[row]);
            }
        }
        (dx, dp)
    }

    fn update(&mut self, grad: &Self) {
        for j in 0..3*H {
            axpy(&mut self.w[j], -RATE, &grad.w[j]);
            axpy(&mut self.u[j], -RATE, &grad.u[j]);
        }
        axpy(&mut self.b, -RATE, &grad.b);
    }
}

#[derive(Clone)]
pub struct Model {
    emb: [[f32; E]; K],
    lower: Layer<E>, upper: Layer<H>,
    head: [Hidden; K], bias: [f32; K],
}
const _: () = assert!(std::mem::size_of::<Model>() <= 64*1024);

impl Model {
    const ZERO: Self = Self { emb: [[0.; E]; K], lower: Layer::ZERO, upper: Layer::ZERO,
        head: [[0.; H]; K], bias: [0.; K] };

    pub fn new() -> Self {
        // Deterministic fan-in-scaled uniform weights, zero biases.
        let mut rng = 0x518a_b2e5u32;
        fn fill<const N: usize>(rows: &mut [[f32; N]], rng: &mut u32) {
            let scale = (3. / N as f32).sqrt();
            for w in rows.iter_mut().flatten() {
                *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
                *w = ((*rng >> 8) as f32 / 8388608. - 1.) * scale;
            }
        }
        let mut m = Self::ZERO;
        fill(&mut m.emb, &mut rng); fill(&mut m.lower.w, &mut rng);
        fill(&mut m.lower.u, &mut rng); fill(&mut m.upper.w, &mut rng);
        fill(&mut m.upper.u, &mut rng); fill(&mut m.head, &mut rng);
        m
    }

    fn logits<const S: bool>(&self, h: &Hidden) -> [f32; K] {
        std::array::from_fn(|j| self.bias[j] + dot::<H,S>(&self.head[j], h))
    }
    pub fn advance(&self, state: &mut [[f32; H]; 2], token: u8) -> u8 {
        fn run<const S: bool>(m: &Model, s: &mut [[f32; H]; 2], t: u8) -> u8 {
            s[0] = m.lower.step::<S>(&m.emb[t as usize], &s[0]).h;
            s[1] = m.upper.step::<S>(&s[0], &s[1]).h;
            let scores = m.logits::<S>(&s[1]);
            let mut best = 0;
            for j in 1..K { if scores[j] > scores[best] { best = j; } }
            best as u8
        }
        if has_sve() { run::<true>(self,state,token) } else { run::<false>(self,state,token) }
    }

    pub fn predict(&self, context: &[u8; T]) -> u8 {
        self.predict_until(context, &mut || false).expect("prediction cannot be cancelled")
    }

    /// Check the worker budget before each recurrent step and the output head.
    pub fn predict_until(&self, context: &[u8; T], stop: &mut impl FnMut() -> bool) -> Option<u8> {
        if has_sve() { self.infer::<true>(context, stop) } else { self.infer::<false>(context, stop) }
    }

    fn infer<const S: bool>(&self, context: &[u8; T], stop: &mut impl FnMut() -> bool) -> Option<u8> {
        let (mut lo, mut hi) = ([0.; H], [0.; H]);
        for &tok in context {
            if stop() { return None; }
            lo = self.lower.step::<S>(&self.emb[tok as usize], &lo).h;
            hi = self.upper.step::<S>(&lo, &hi).h;
        }
        if stop() { return None; }
        let scores = self.logits::<S>(&hi);
        let mut best = 0;
        for j in 1..K { if scores[j] > scores[best] { best = j; } }
        Some(best as u8)
    }

    /// SGD on mean cross entropy: input context[t] predicts context[t+1]
    /// (or `target` at t=31). Each window starts with zero hidden states.
    pub fn train(&mut self, context: &[u8; T], target: u8) {
        self.train_until(context, target, &mut || false);
    }

    /// Returns false without changing any parameters if the budget expires.
    pub fn train_until(&mut self, context: &[u8; T], target: u8, stop: &mut impl FnMut() -> bool) -> bool {
        self.train_weighted_until(context, target, 1., stop)
    }

    /// Multiply each abstain label's cross entropy by `weight` before averaging
    /// over the 32 steps. Cancellation discards the entire window's gradient.
    pub fn train_weighted_until(&mut self, context: &[u8; T], target: u8, weight: f32, stop: &mut impl FnMut() -> bool) -> bool {
        assert!(weight.is_finite() && weight >= 0.);
        let mut grad = Self::ZERO;
        let complete = if has_sve() { self.gradient::<true>(context, target, weight, &mut grad, stop) }
        else { self.gradient::<false>(context, target, weight, &mut grad, stop) };
        if !complete || stop() { return false; }
        self.lower.update(&grad.lower); self.upper.update(&grad.upper);
        for j in 0..K {
            axpy(&mut self.emb[j], -RATE, &grad.emb[j]);
            axpy(&mut self.head[j], -RATE, &grad.head[j]);
        }
        axpy(&mut self.bias, -RATE, &grad.bias);
        true
    }

    fn gradient<const S: bool>(&self, context: &[u8; T], target: u8, weight: f32, g: &mut Self, stop: &mut impl FnMut() -> bool) -> bool {
        let (mut lo, mut hi) = ([START; T+1], [START; T+1]);
        for (t, &tok) in context.iter().enumerate() {
            if stop() { return false; }
            lo[t+1] = self.lower.step::<S>(&self.emb[tok as usize], &lo[t].h);
            hi[t+1] = self.upper.step::<S>(&lo[t+1].h, &hi[t].h);
        }
        let (mut dlo, mut dhi) = ([0.; H], [0.; H]);
        for t in (0..T).rev() {
            if stop() { return false; }
            let tok = context[t] as usize;
            let y = context.get(t+1).copied().unwrap_or(target) as usize;
            let mut p = self.logits::<S>(&hi[t+1].h);
            let max = p.iter().copied().fold(f32::NEG_INFINITY, f32::max);
            for v in &mut p { *v = (*v - max).exp(); }
            let inv_sum = 1. / p.iter().sum::<f32>();
            for v in &mut p { *v *= inv_sum; }
            p[y] -= 1.;
            let scale = if y == 63 { weight } else { 1. } / T as f32;
            for (j, d) in p.into_iter().enumerate() {
                let d = d * scale;
                g.bias[j] += d;
                axpy(&mut g.head[j], d, &hi[t+1].h);
                axpy(&mut dhi, d, &self.head[j]);
            }
            let (vertical, carry) = self.upper.backward(&mut g.upper, &hi[t+1], &lo[t+1].h, &hi[t].h, &dhi);
            dhi = carry;
            axpy(&mut dlo, 1., &vertical);
            let (dx, carry) = self.lower.backward(&mut g.lower, &lo[t+1], &self.emb[tok], &lo[t].h, &dlo);
            dlo = carry;
            axpy(&mut g.emb[tok], 1., &dx);
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loss(m: &Model, ctx: &[u8; T], last: u8) -> f64 {
        let (mut a, mut b) = ([0.; H], [0.; H]);
        let mut nll = 0.;
        for (t, &tok) in ctx.iter().enumerate() {
            a = m.lower.step::<false>(&m.emb[tok as usize], &a).h;
            b = m.upper.step::<false>(&a, &b).h;
            let s = m.logits::<false>(&b).map(f64::from);
            let y = ctx.get(t+1).copied().unwrap_or(last) as usize;
            nll += s.iter().map(|x| x.exp()).sum::<f64>().ln() - s[y];
        }
        nll / T as f64
    }

    #[test]
    fn full_window_gradients_match_centered_differences() {
        let mut m = Model::new();
        let ctx = std::array::from_fn(|t| (t % 3) as u8);
        let mut g = Model::ZERO;
        assert!(m.gradient::<false>(&ctx, 63, 1., &mut g, &mut || false));
        // Check the largest-magnitude component of each gate's row block,
        // not a near-zero component that would pass on absolute tolerance.
        fn largest<const N: usize>(rows: &[[f32; N]]) -> (usize, usize) {
            (0..rows.len()).flat_map(|i| (0..N).map(move |j| (i,j)))
                .max_by(|&(i,j), &(k,l)| rows[i][j].abs().total_cmp(&rows[k][l].abs())).unwrap()
        }
        macro_rules! check { ($($field:tt)+) => {{
            let orig = m.$($field)+;
            let eps = 0.008;
            m.$($field)+ = orig + eps;
            let plus = loss(&m, &ctx, 63);
            m.$($field)+ = orig - eps;
            let minus = loss(&m, &ctx, 63);
            m.$($field)+ = orig;
            let fd = (plus-minus) / (2.*f64::from(eps));
            let grad = f64::from(g.$($field)+);
            assert!((fd-grad).abs() < 2e-6 + 0.015*grad.abs(), "{}: {fd} vs {grad}", stringify!($($field)+));
        }}; }
        macro_rules! gates { ($layer:ident, $mat:ident) => {
            for k in 0..3 {
                let (i,j) = largest(&g.$layer.$mat[k*H..(k+1)*H]);
                let i = i + k*H;
                check!($layer.$mat[i][j]);
            }
        }; }
        gates!(lower,w); gates!(lower,u); gates!(upper,w); gates!(upper,u);
        let (i,j) = largest(&g.emb); check!(emb[i][j]);
        let (i,j) = largest(&g.head); check!(head[i][j]);
        check!(bias[63]);
    }

    #[test]
    fn learns_two_distinct_contexts() {
        let mut m = Model::new();
        let a = [7; T]; let b = [44; T];
        let before = loss(&m, &a, 7) + loss(&m, &b, 44);
        for _ in 0..24 { m.train(&a, 7); m.train(&b, 44); }
        assert_eq!((m.predict(&a), m.predict(&b)), (7,44));
        assert!(loss(&m, &a, 7) + loss(&m, &b, 44) < before * 0.7);
    }

    #[test]
    fn prediction_can_stop_mid_window() {
        let m = Model::new();
        let ctx = [3; T];
        let mut calls = 0;
        assert_eq!(m.predict_until(&ctx, &mut || { calls += 1; calls == 5 }), None);
        assert_eq!(m.predict_until(&ctx, &mut || false), Some(m.predict(&ctx)));
    }

    #[test]
    fn interrupted_window_does_not_commit() {
        let ctx = [21; T];
        let mut m = Model::new();
        let before = loss(&m, &ctx, 21);
        for cutoff in [1, T+4, 2*T+1] {
            let mut calls = 0;
            assert!(!m.train_until(&ctx, 21, &mut || { calls += 1; calls >= cutoff }));
            assert_eq!(loss(&m, &ctx, 21), before);
        }
    }

    #[test]
    fn dispatched_dot_matches_double_precision_with_tail() {
        let a: [f32; 35] = std::array::from_fn(|i| (i as f32-16.) / 13.);
        let b: [f32; 35] = std::array::from_fn(|i| (i as f32+2.) / 11.);
        let reference: f64 = a.iter().zip(b).map(|(&x,y)| f64::from(x)*f64::from(y)).sum();
        let got = if has_sve() { dot::<35,true>(&a,&b) } else { dot::<35,false>(&a,&b) };
        assert!((f64::from(got)-reference).abs() < 1e-5);
    }
}
