use std::fs;
use std::path::{Path, PathBuf};

use qlic_decoder::{Error, Limits, validate};

fn fixtures() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

#[test]
fn validates_every_public_family() {
    for name in [
        "native.qlic",
        "animation.qlic",
        "wide-u16-16-rgba.qlic",
        "described-u16-8-srgb-rgb.qlic",
        "hdr-u16-10-hlg-rgb.qlic",
        "hdr-u16-12-pq-rgba.qlic",
    ] {
        let bytes = fs::read(fixtures().join(name)).expect("read retained fixture");
        validate(&bytes, &Limits::default()).expect("validate retained fixture");
    }
}

#[test]
fn rejects_damage_and_limits() {
    let mut bytes = fs::read(fixtures().join("native.qlic")).expect("read native fixture");
    let middle = bytes.len() / 2;
    bytes[middle] ^= 0x40;
    assert!(validate(&bytes, &Limits::default()).is_err());

    let original = fs::read(fixtures().join("native.qlic")).expect("read native fixture");
    let limits = Limits {
        max_pixels: 1,
        ..Limits::default()
    };
    assert!(matches!(
        validate(&original, &limits),
        Err(Error::LimitExceeded { .. })
    ));
}
