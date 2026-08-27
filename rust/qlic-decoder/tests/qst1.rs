use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Error, LimitKind, Limits, crc32, decode_rgba, parse_qst1};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(fixture_dir().join(name)).unwrap()
}

fn rewrite_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn rewrite_inner_and_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let mut qst1 = bytes[28..footer].to_vec();
    qst1[26..30].fill(0);
    let checksum = crc32(&qst1).to_le_bytes();
    bytes[54..58].copy_from_slice(&checksum);
    rewrite_outer_crc(bytes);
}

#[test]
fn parses_and_decodes_the_retained_mode40_native_fixture() {
    let file = fixture("native.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (64, 64));
    assert_eq!(stream.info.channels, 1);
    assert_eq!(stream.info.flags, 0);
    assert_eq!(stream.info.mode, 40);
    assert_eq!(stream.info.transform, 0);
    assert_eq!(stream.info.tile_log, 1);
    assert_eq!(stream.info.adaptation, 5);
    assert_eq!(stream.info.pixel_crc32, 0x248a_6029);
    assert_eq!(stream.info.payload_size, 11);
    assert_eq!(stream.info.container_crc32, 0xba23_e05b);
    assert!(stream.palette.is_empty());
    assert_eq!(stream.payload.len(), 11);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (64, 64));
    assert_eq!(decoded.rgba.len(), 64 * 64 * 4);
    assert_eq!(crc32(&decoded.rgba), 0xb385_d194);
}

#[test]
fn parses_and_decodes_the_retained_mode52_transform38_fixture() {
    let file = fixture("normal-map-quadratic.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!(stream.info.mode, 52);
    assert_eq!(stream.info.transform, 38);
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.tile_log, 4);
    assert_eq!(stream.info.adaptation, 4);
    assert_eq!(stream.info.pixel_crc32, 0x5c36_171a);
    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (128, 256));
    assert_eq!(decoded.rgba.len(), 128 * 256 * 4);
    assert_eq!(crc32(&decoded.rgba), 0x9122_c65a);
    // Fixed samples cross-checked against the production C decoder's PPM output.
    for (pixel, expected) in [
        (0, [97, 115, 251, 255]),
        (1, [95, 116, 250, 255]),
        (127, [174, 84, 238, 255]),
        (128, [96, 116, 251, 255]),
        (16_384, [33, 64, 184, 255]),
        (32_767, [154, 148, 250, 255]),
    ] {
        assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], expected);
    }
}

#[test]
fn rejects_inner_checksum_payload_length_and_limits() {
    let original = fixture("native.qlic");
    let mut inner_corrupt = original.clone();
    inner_corrupt[58] ^= 1;
    rewrite_outer_crc(&mut inner_corrupt);
    assert!(matches!(
        decode_rgba(&inner_corrupt, &Limits::default()),
        Err(Error::InvalidQst1("container checksum mismatch"))
    ));

    let outer = Container::parse(&original, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qst1(
            &outer.payload[..outer.payload.len() - 1],
            &Limits::default()
        ),
        Err(Error::InvalidQst1(
            "declared payload size does not match stream"
        ))
    ));

    let limits = Limits {
        max_pixels: 4_095,
        ..Limits::default()
    };
    assert!(matches!(
        parse_qst1(outer.payload, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Pixels,
            actual: 4_096,
            ..
        })
    ));
}

#[test]
fn mode40_public_decode_never_panics_on_truncation_or_rechecksummed_mutation() {
    let original = fixture("native.qlic");
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 18,
        max_decoded_bytes: 1 << 20,
        ..Limits::default()
    };
    for length in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on native prefix of {length} bytes"
        );
    }
    for offset in 28..original.len() - 4 {
        let mut mutated = original.clone();
        mutated[offset] ^= 0xa5;
        rewrite_inner_and_outer_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(result.is_ok(), "panicked on native mutation at {offset}");
    }
}

#[test]
fn mode52_public_decode_never_panics_on_truncation_or_rechecksummed_mutation() {
    let original = fixture("normal-map-quadratic.qlic");
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 18,
        max_decoded_bytes: 1 << 20,
        ..Limits::default()
    };
    for length in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&original[..length], &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on mode-52 prefix of {length} bytes"
        );
    }

    let mut offsets = (28..58).collect::<Vec<_>>();
    offsets.extend((58..original.len() - 4).step_by(31));
    offsets.push(original.len() - 5);
    offsets.sort_unstable();
    offsets.dedup();
    for offset in offsets {
        let mut mutated = original.clone();
        mutated[offset] ^= 0x5a;
        rewrite_inner_and_outer_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &limits);
        }));
        assert!(
            result.is_ok(),
            "panicked on rechecksummed mode-52 mutation at {offset}"
        );
    }
}

#[test]
fn mode52_keeps_neighboring_modes_channels_and_allocations_explicit() {
    let original = fixture("normal-map-quadratic.qlic");

    let mut unsupported_channels = original.clone();
    unsupported_channels[28 + 12] = 1;
    unsupported_channels[28 + 15] = 0;
    rewrite_inner_and_outer_crc(&mut unsupported_channels);
    assert!(matches!(
        decode_rgba(&unsupported_channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 52,
            channels: 1
        })
    ));

    let mut unsupported_mode = original.clone();
    unsupported_mode[28 + 14] = 51;
    rewrite_inner_and_outer_crc(&mut unsupported_mode);
    assert!(matches!(
        decode_rgba(&unsupported_mode, &Limits::default()),
        Err(Error::UnsupportedQst1Mode(51))
    ));

    let limits = Limits {
        max_decoded_bytes: 128 * 256 * 4 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 131_072,
            ..
        })
    ));
}
