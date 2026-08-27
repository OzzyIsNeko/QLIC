use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Error, LimitKind, Limits, crc32, decode_rgba, parse_qst1};

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn retained_profile(relative: &str) -> Vec<u8> {
    fs::read(repository_root().join(relative)).unwrap()
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

fn mutation_offsets(length: usize, payload_stride: usize) -> Vec<usize> {
    let mut offsets = (28..58).collect::<Vec<_>>();
    offsets.extend((58..length - 4).step_by(payload_stride));
    offsets.push(length - 5);
    offsets.sort_unstable();
    offsets.dedup();
    offsets
}

#[test]
fn decodes_retained_mode45_rgba_tile_stream_exactly() {
    let file = retained_profile("retained/mode45-current-streams/0013.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (1024, 1024));
    assert_eq!(stream.info.channels, 4);
    assert_eq!(stream.info.mode, 45);
    assert_eq!(stream.info.transform, 2);
    assert_eq!(stream.info.tile_log, 3);
    assert_eq!(stream.info.adaptation, 5);
    assert_eq!(stream.info.pixel_crc32, 0x069a_52be);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (1024, 1024));
    assert_eq!(crc32(&decoded.rgba), 0x069a_52be);
    for pixel in [0, 1, 1023, 1024, 524_288, 1_048_575] {
        assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], [0, 0, 0, 0]);
    }
}

#[test]
fn decodes_retained_mode53_rgb_refined_stream_exactly() {
    let file = retained_profile("retained/mode53-post-alpha-current-streams/0092.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (128, 512));
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.mode, 53);
    assert_eq!(stream.info.transform, 5);
    assert_eq!(stream.info.tile_log, 4);
    assert_eq!(stream.info.adaptation, 5);
    assert_eq!(stream.info.pixel_crc32, 0x9955_e6a9);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (128, 512));
    assert_eq!(crc32(&decoded.rgba), 0x0377_d425);
    for (pixel, expected) in [
        (0, [50, 49, 52, 255]),
        (1, [49, 49, 49, 255]),
        (127, [59, 55, 53, 255]),
        (128, [61, 58, 60, 255]),
        (32_768, [32, 30, 29, 255]),
        (65_535, [53, 51, 50, 255]),
    ] {
        assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], expected);
    }
}

#[test]
fn decodes_retained_mode45_rgb_single_predictor_tile_exactly() {
    let file = retained_profile("retained/mode45-current-streams/0027.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (1200, 899));
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.mode, 45);
    assert_eq!(stream.info.transform, 7);
    assert_eq!(stream.info.tile_log, 0);
    assert_eq!(stream.info.adaptation, 5);
    assert_eq!(stream.info.pixel_crc32, 0x581b_b714);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    for (pixel, expected) in [
        (0, [229, 229, 229, 255]),
        (1, [229, 229, 229, 255]),
        (1_199, [232, 232, 232, 255]),
        (1_200, [227, 227, 227, 255]),
        (539_400, [2, 2, 2, 255]),
        (1_078_799, [230, 230, 230, 255]),
    ] {
        assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], expected);
    }
}

#[test]
fn common_native_modes_enforce_limits_and_keep_neighboring_modes_explicit() {
    let mode45 = retained_profile("retained/mode45-current-streams/0013.qlic");
    let mode45_limits = Limits {
        max_decoded_bytes: 1024 * 1024 * 4 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&mode45, &mode45_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 4_194_304,
            ..
        })
    ));

    let mut unsupported_mode45_channels = mode45.clone();
    unsupported_mode45_channels[28 + 12] = 1;
    unsupported_mode45_channels[28 + 15] = 0;
    rewrite_inner_and_outer_crc(&mut unsupported_mode45_channels);
    assert!(matches!(
        decode_rgba(&unsupported_mode45_channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 45,
            channels: 1
        })
    ));

    let mode53 = retained_profile("retained/mode53-post-alpha-current-streams/0092.qlic");
    let mode53_limits = Limits {
        max_decoded_bytes: 128 * 512 * 4 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&mode53, &mode53_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 262_144,
            ..
        })
    ));

    let mut unsupported_mode53_channels = mode53.clone();
    unsupported_mode53_channels[28 + 12] = 1;
    unsupported_mode53_channels[28 + 15] = 0;
    rewrite_inner_and_outer_crc(&mut unsupported_mode53_channels);
    assert!(matches!(
        decode_rgba(&unsupported_mode53_channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 53,
            channels: 1
        })
    ));

    let mut unsupported_mode53_gray = mode53.clone();
    unsupported_mode53_gray[28 + 13] = 1;
    unsupported_mode53_gray[28 + 15] = 0;
    rewrite_inner_and_outer_crc(&mut unsupported_mode53_gray);
    assert!(matches!(
        decode_rgba(&unsupported_mode53_gray, &Limits::default()),
        Err(Error::UnsupportedQst1Flags { mode: 53, flags: 1 })
    ));

    let mut unsupported_neighbor = mode53.clone();
    unsupported_neighbor[28 + 14] = 54;
    rewrite_inner_and_outer_crc(&mut unsupported_neighbor);
    assert!(matches!(
        decode_rgba(&unsupported_neighbor, &Limits::default()),
        Err(Error::UnsupportedQst1Transform {
            mode: 54,
            transform: 5
        })
    ));
}

#[test]
fn common_native_public_decode_rejects_sampled_damage_without_panicking() {
    let cases = [
        (
            "mode-45",
            retained_profile("retained/mode45-current-streams/0013.qlic"),
            4_001,
        ),
        (
            "mode-53",
            retained_profile("retained/mode53-post-alpha-current-streams/0092.qlic"),
            8_191,
        ),
    ];
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 2 << 20,
        max_decoded_bytes: 8 << 20,
        ..Limits::default()
    };

    for (name, original, stride) in cases {
        for length in [0, 1, 27, 28, 57, original.len() / 2, original.len() - 1] {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&original[..length], &limits);
            }));
            assert!(
                result.is_ok(),
                "panicked on {name} prefix of {length} bytes"
            );
        }
        for offset in mutation_offsets(original.len(), stride) {
            let mut mutated = original.clone();
            mutated[offset] ^= 0xa5;
            rewrite_inner_and_outer_crc(&mut mutated);
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&mutated, &limits);
            }));
            assert!(
                result.is_ok(),
                "panicked on rechecksummed {name} mutation at {offset}"
            );
        }
    }
}
