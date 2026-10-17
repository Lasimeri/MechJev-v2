//! Jev's published request and answer pairs (TypeSafe's docs,
//! `evidence/published_pairs.json`) and a few structural oddities
//! (`evidence/invariants.json`), from Mechanical Jev: the examples the TUI
//! offers, each with Jev's own answer beside the engine's. See evidence.md.

use serde_json::Value;

const PAIRS: &str = include_str!("../evidence/published_pairs.json");
const INVARIANTS: &str = include_str!("../evidence/invariants.json");

/// Every published pair, the invariant examples after them.
pub fn all() -> Vec<Value> {
    let mut v: Vec<Value> = serde_json::from_str(PAIRS).unwrap_or_default();
    v.extend(serde_json::from_str::<Vec<Value>>(INVARIANTS).unwrap_or_default());
    v
}
