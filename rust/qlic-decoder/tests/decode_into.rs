use std::fs;
use std::path::{Path, PathBuf};

use qlic_decoder::{Error, Limits, decode_rgba, decode_rgba_into, parse_info};

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(relative: &str) -> Vec<u8> {
    fs::read(repository_root().join(relative)).unwrap()
}

fn check_into(relative: &str) {
    let encoded = fixture(relative);
    let limits = Limits::default();
    let info = parse_info(&encoded, &limits).unwrap();
    let expected = decode_rgba(&encoded, &limits).unwrap();
    let mut destination = vec![0xa5; expected.rgba.len()];
    let shape = decode_rgba_into(&encoded, &limits, &mut destination).unwrap();
    assert_eq!(shape, (info.width, info.height));
    assert_eq!(destination, expected.rgba);

    destination.fill(0x5a);
    let second = decode_rgba_into(&encoded, &limits, &mut destination).unwrap();
    assert_eq!(second, shape);
    assert_eq!(destination, expected.rgba);
}

#[test]
fn decodes_native_and_tile_streams_into_reused_storage() {
    check_into("native.qlic");
    check_into("retained/native-rgba-tile-bands-final-pairs/0000-candidate.qlic");
}

#[test]
fn decodes_ordinary_stream_into_reused_storage() {
    check_into("palette.qlic");
}

#[test]
fn rejects_wrong_destination_length_before_entropy_decode() {
    let encoded = fixture("native.qlic");
    let mut destination = vec![0; 64 * 64 * 4 - 1];
    assert_eq!(
        decode_rgba_into(&encoded, &Limits::default(), &mut destination),
        Err(Error::InvalidPixelData(
            "RGBA destination does not match the image shape"
        ))
    );
}
