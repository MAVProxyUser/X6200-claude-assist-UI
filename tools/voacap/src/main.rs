// SPDX-License-Identifier: LGPL-2.1-or-later
//
// GoldCorpHF's voacapl: the program Pat runs for its VOACAP column (the Winlink gateway list's predicted path
// quality), as Pat runs voacapl, on hfcast-engine (Apache-2.0, a Rust port of VOACAP checked cell by cell against
// the Fortran, its coefficients compiled in: no itshfbc tree, no Fortran on the radio).
//
//     voacapl -v
//     voacapl [--run-dir=DIR] [SILENT] ITSHFBC INPUT OUTPUT
//
// INPUT (in DIR, else the working directory) is Pat's deck (Pat v1.0.1 internal/propagation/voacap/encode.go): one
// path (CIRCUIT, LABEL), the month and year, the sunspot number, the SYSTEM card's noise and required SNR, the
// ANTENNA cards' power, the FREQUENCY card, METHOD 30. OUTPUT gets the listing voacapl prints for it, by
// hfcast's api::listing (all 24 hours; Pat takes the hour it asked), with Pat's own deck echoed at its top, where
// Pat's parser reads back the hour and frequency it asked for. ITSHFBC is not read.
//
// The power: Pat asks at its fixed 50 W; X6100_VOACAP_W (default 7) caps it, so the column is for what the radio
// sends. hfcast fixes the SYSTEM card's minimum angle (0.10, as Pat's deck), required reliability (90) and
// multipath tolerances; Pat's score reads REL, SNR, the take-off angle and the mode, which follow the required SNR
// and noise passed here.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use hfcast::api::{self, FoF2Model, Request, Site, Task};

const DEFAULT_W: f64 = 7.0;

fn version() -> String {
    format!("voacapl (GoldCorpHF {}, hfcast-engine {})", env!("CARGO_PKG_VERSION"), hfcast_version())
}

fn hfcast_version() -> &'static str {
    option_env!("HFCAST_VERSION").unwrap_or("2.14.0")
}

/// The text of a card's columns from..to (0-based, end exclusive), trimmed; "" past the line's end
fn cols(line: &str, from: usize, to: usize) -> &str {
    let b = line.as_bytes();
    if from >= b.len() {
        return "";
    }
    line.get(from..to.min(b.len())).unwrap_or("").trim()
}

fn num(s: &str, what: &str) -> Result<f64, String> {
    s.trim().parse::<f64>().map_err(|_| format!("{what}: '{s}' is not a number"))
}

/// The numbers after the keyword, as whitespace separates them
fn tokens(line: &str) -> Vec<&str> {
    line.get(10..).unwrap_or("").split_whitespace().collect()
}

/// CIRCUIT: four numbers, each followed by its hemisphere (N S E W), however close they run
fn circuit(line: &str) -> Result<[f64; 4], String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    for ch in line.get(10..).unwrap_or("").chars() {
        match ch {
            '0'..='9' | '.' | '-' => cur.push(ch),
            'N' | 'S' | 'E' | 'W' if !cur.is_empty() => {
                let v = num(&cur, "CIRCUIT")?;
                out.push(if ch == 'S' || ch == 'W' { -v } else { v });
                cur.clear();
            }
            _ => cur.clear(),
        }
        if out.len() == 4 {
            break;
        }
    }
    if out.len() != 4 {
        return Err("CIRCUIT: not two latitudes and longitudes".into());
    }
    Ok([out[0], out[1], out[2], out[3]])
}

fn parse_deck(deck: &str, cap_w: f64) -> Result<Request, String> {
    let (mut tx, mut rx) = (String::from("TX"), String::from("RX"));
    let mut path = None;
    let (mut year, mut month, mut ssn) = (2026u32, 0u32, None);
    let (mut noise, mut snr) = (145.0, 20.0);
    let mut fof2 = FoF2Model::Ccir;
    let mut mult = [1.0, 1.0, 1.0, 0.0];
    let mut watts: Option<f64> = None;
    let mut freqs = Vec::new();
    let mut method = 30;

    for line in deck.lines() {
        let key = cols(line, 0, 10).to_ascii_uppercase();
        let key = key.split_whitespace().next().unwrap_or("");
        match key {
            "LABEL" => {
                tx = cols(line, 10, 30).to_string();
                rx = cols(line, 30, 50).to_string();
            }
            "CIRCUIT" => path = Some(circuit(line)?),
            "MONTH" => {
                // Five-column fields that run together from October on ("MONTH      202611.00")
                year = num(cols(line, 10, 15), "MONTH year")? as u32;
                month = num(cols(line, 15, 20), "MONTH month")? as u32;
            }
            "SUNSPOT" => ssn = Some(num(tokens(line).first().copied().unwrap_or(""), "SUNSPOT")?),
            "SYSTEM" => {
                // Five-column fields: power, man-made noise, minimum angle, required reliability, required SNR, ...
                noise = num(cols(line, 15, 20), "SYSTEM noise")?;
                snr = num(cols(line, 30, 35), "SYSTEM required SNR")?;
            }
            "COEFFS" => {
                if cols(line, 10, 20).to_ascii_uppercase().starts_with("URSI") {
                    fof2 = FoF2Model::Ursi;
                }
            }
            "FPROB" => {
                for (k, m) in mult.iter_mut().enumerate() {
                    let f = cols(line, 10 + 5 * k, 15 + 5 * k);
                    if !f.is_empty() {
                        *m = num(f, "FPROB")?;
                    }
                }
            }
            "ANTENNA" => {
                // The last field: the power in kW (Pat writes it on both cards; the transmitter's counts)
                if let Some(kw) = line.rsplit_once(']').and_then(|(_, r)| r.split_whitespace().last()) {
                    let w = num(kw, "ANTENNA power")? * 1000.0;
                    if w > 0.0 && watts.is_none() {
                        watts = Some(w);
                    }
                }
            }
            "FREQUENCY" => {
                for v in tokens(line) {
                    let f = num(v, "FREQUENCY")?;
                    if f > 0.0 {
                        freqs.push(f);
                    }
                }
            }
            "METHOD" => method = num(tokens(line).first().copied().unwrap_or(""), "METHOD")? as u32,
            _ => {}
        }
    }
    if method != 30 {
        return Err(format!("METHOD {method}: only method 30 (Pat's) is answered here"));
    }
    let p = path.ok_or("no CIRCUIT card")?;
    if freqs.is_empty() {
        return Err("no FREQUENCY card".into());
    }
    Ok(Request {
        tx: Site { name: tx, lat_deg: p[0], lon_deg: p[1] },
        rx: Site { name: rx, lat_deg: p[2], lon_deg: p[3] },
        month,
        year,
        ssn: ssn.ok_or("no SUNSPOT card")?,
        power_watts: watts.unwrap_or(cap_w).min(cap_w),
        freqs_mhz: freqs,
        required_snr_db: snr,
        noise_dbw: noise,
        fof2,
        layer_multipliers: mult,
        tx_antennas: Vec::new(),        // the isotrope over 2-30 MHz, as Pat's blank antenna names
        rx_antennas: Vec::new(),
        ionosphere: Default::default(),
        model: Default::default(),
        numerics: Default::default(),
    })
}

/// The listing with Pat's own deck echoed in place of hfcast's: between the column ruler and the first page header
fn with_deck(listing: &str, deck: &str) -> String {
    let mut out = String::with_capacity(listing.len() + deck.len());
    let mut lines = listing.lines();
    let mut echoed = false;
    for l in lines.by_ref() {
        out.push_str(l);
        out.push('\n');
        if l.trim_start().starts_with("1234567890") {
            out.push('\n');
            for d in deck.lines() {
                out.push(' ');
                out.push_str(d.trim_end());
                out.push('\n');
            }
            echoed = true;
            break;
        }
    }
    if !echoed {
        return listing.to_string();
    }
    let mut skipping = true;
    for l in lines {
        if skipping && (l.contains("Coefficients") || l.contains("METHOD")) && !l.trim_start().starts_with("METHOD") {
            skipping = false;
        }
        if !skipping {
            out.push_str(l);
            out.push('\n');
        }
    }
    out
}

fn run(args: &[String]) -> Result<(), String> {
    let mut dir: Option<PathBuf> = None;
    let mut rest = Vec::new();
    for a in args {
        if let Some(d) = a.strip_prefix("--run-dir=") {
            dir = Some(PathBuf::from(d));
        } else if a != "SILENT" {
            rest.push(a.as_str());
        }
    }
    if rest.len() != 3 {
        return Err("usage: voacapl [--run-dir=DIR] ITSHFBC INPUT OUTPUT, or -v".into());
    }
    let base = dir.unwrap_or_else(|| PathBuf::from("."));
    let at = |name: &str| if Path::new(name).is_absolute() { PathBuf::from(name) } else { base.join(name) };
    let deck = fs::read_to_string(at(rest[1])).map_err(|e| format!("{}: {e}", rest[1]))?;
    let cap = env::var("X6100_VOACAP_W").ok().and_then(|v| v.parse::<f64>().ok()).filter(|w| *w > 0.0)
        .unwrap_or(DEFAULT_W);
    let req = parse_deck(&deck, cap)?;
    let listing = api::listing(Path::new(hfcast::voacap::data::EMBEDDED), &req, Task::Systems)?;
    fs::write(at(rest[2]), with_deck(&listing, &deck)).map_err(|e| format!("{}: {e}", rest[2]))
}

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("-v") {
        println!("{}", version());
        return ExitCode::SUCCESS;
    }
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("voacapl: {e}");
            ExitCode::FAILURE
        }
    }
}
