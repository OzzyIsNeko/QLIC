use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Error, LimitKind, Limits, crc32, decode_rgba, parse_qst1};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/final-corpus")
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

struct FinalCase {
    name: &'static str,
    bytes: usize,
    width: u32,
    height: u32,
    mode: u8,
    transform: u8,
    rgb_crc32: u32,
    rgba_crc32: u32,
    samples: &'static [(usize, [u8; 4])],
}

const CASES: &[FinalCase] = &[
    FinalCase {
        name: "2612-mode39-t7.qlic",
        bytes: 5_014,
        width: 256,
        height: 256,
        mode: 39,
        transform: 7,
        rgb_crc32: 0xbbaf_7a49,
        rgba_crc32: 0x2259_7328,
        samples: &[
            (6_717, [4, 4, 4, 255]),
            (9_306, [33, 33, 30, 255]),
            (10_712, [22, 62, 161, 255]),
        ],
    },
    FinalCase {
        name: "2871-mode39-t2.qlic",
        bytes: 3_161,
        width: 512,
        height: 512,
        mode: 39,
        transform: 2,
        rgb_crc32: 0xdf4a_317f,
        rgba_crc32: 0xc31d_4c35,
        samples: &[
            (0, [5, 5, 5, 255]),
            (51_145, [28, 28, 28, 255]),
            (262_143, [3, 3, 3, 255]),
        ],
    },
    FinalCase {
        name: "2875-mode39-t2.qlic",
        bytes: 1_908,
        width: 512,
        height: 512,
        mode: 39,
        transform: 2,
        rgb_crc32: 0xb500_7ed5,
        rgba_crc32: 0x9379_5673,
        samples: &[
            (0, [3, 4, 4, 255]),
            (205_009, [4, 4, 4, 255]),
            (262_143, [7, 7, 8, 255]),
        ],
    },
    FinalCase {
        name: "1096-mode40-t3.qlic",
        bytes: 1_675,
        width: 106,
        height: 133,
        mode: 40,
        transform: 3,
        rgb_crc32: 0xd9d6_8153,
        rgba_crc32: 0xdc52_90cb,
        samples: &[
            (182, [0, 128, 0, 255]),
            (6_795, [192, 192, 192, 255]),
            (13_729, [255, 255, 0, 255]),
        ],
    },
    FinalCase {
        name: "1723-mode40-t4.qlic",
        bytes: 308,
        width: 128,
        height: 256,
        mode: 40,
        transform: 4,
        rgb_crc32: 0xb2f5_dc29,
        rgba_crc32: 0xead1_5dd4,
        samples: &[
            (3_632, [6, 25, 16, 255]),
            (6_474, [31, 0, 0, 255]),
            (10_674, [3, 0, 0, 255]),
        ],
    },
];

#[test]
fn decodes_the_five_final_current_corpus_streams_exactly() {
    assert_eq!(CASES.iter().map(|case| case.bytes).sum::<usize>(), 12_066);
    for case in CASES {
        let file = fixture(case.name);
        assert_eq!(file.len(), case.bytes);
        let outer = Container::parse(&file, &Limits::default()).unwrap();
        let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
        assert_eq!(
            (stream.info.width, stream.info.height),
            (case.width, case.height)
        );
        assert_eq!(stream.info.channels, 3);
        assert_eq!(stream.info.flags, 0);
        assert_eq!(stream.info.mode, case.mode);
        assert_eq!(stream.info.transform, case.transform);
        assert_eq!(stream.info.tile_log, if case.mode == 39 { 0 } else { 1 });
        assert_eq!(stream.info.adaptation, 4);
        assert_eq!(stream.info.pixel_crc32, case.rgb_crc32);

        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (case.width, case.height));
        assert_eq!(crc32(&decoded.rgba), case.rgba_crc32);
        for &(pixel, expected) in case.samples {
            assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], expected);
        }
    }
}

#[test]
fn final_event_and_pattern_slices_enforce_limits_and_reject_neighbors() {
    let event = fixture("2871-mode39-t2.qlic");
    for limits in [
        Limits {
            max_pixels: 512 * 512 - 1,
            ..Limits::default()
        },
        Limits {
            max_decoded_bytes: 512 * 512 * 4 - 1,
            ..Limits::default()
        },
    ] {
        assert!(matches!(
            decode_rgba(&event, &limits),
            Err(Error::LimitExceeded { .. })
        ));
    }

    let mut event_transform = event.clone();
    event_transform[28 + 15] = 3;
    rewrite_inner_and_outer_crc(&mut event_transform);
    assert!(matches!(
        decode_rgba(&event_transform, &Limits::default()),
        Err(Error::UnsupportedQst1Transform {
            mode: 39,
            transform: 3
        })
    ));

    let mut event_flags = event.clone();
    event_flags[28 + 13] = 4;
    rewrite_inner_and_outer_crc(&mut event_flags);
    assert!(matches!(
        decode_rgba(&event_flags, &Limits::default()),
        Err(Error::UnsupportedQst1Flags { mode: 39, flags: 4 })
    ));

    let mut rgba_transform = event.clone();
    rgba_transform[28 + 12] = 4;
    rgba_transform[28 + 15] = 7;
    rewrite_inner_and_outer_crc(&mut rgba_transform);
    assert!(matches!(
        decode_rgba(&rgba_transform, &Limits::default()),
        Err(Error::UnsupportedQst1Transform {
            mode: 39,
            transform: 7
        })
    ));

    let pattern = fixture("1096-mode40-t3.qlic");
    let pattern_limits = Limits {
        max_decoded_bytes: 106 * 133 * 4 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&pattern, &pattern_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            ..
        })
    ));

    let mut pattern_transform = pattern.clone();
    pattern_transform[28 + 15] = 2;
    rewrite_inner_and_outer_crc(&mut pattern_transform);
    assert!(matches!(
        decode_rgba(&pattern_transform, &Limits::default()),
        Err(Error::UnsupportedQst1Transform {
            mode: 40,
            transform: 2
        })
    ));

    let mut pattern_channels = pattern.clone();
    pattern_channels[28 + 12] = 4;
    rewrite_inner_and_outer_crc(&mut pattern_channels);
    assert!(matches!(
        decode_rgba(&pattern_channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 40,
            channels: 4
        })
    ));

    let mut pattern_flags = pattern.clone();
    pattern_flags[28 + 13] = 4;
    rewrite_inner_and_outer_crc(&mut pattern_flags);
    assert!(matches!(
        decode_rgba(&pattern_flags, &Limits::default()),
        Err(Error::UnsupportedQst1Flags { mode: 40, flags: 4 })
    ));
}

#[test]
fn final_event_and_pattern_streams_never_panic_on_truncation_or_repaired_damage() {
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 20,
        max_decoded_bytes: 4 << 20,
        ..Limits::default()
    };
    for case in CASES {
        let original = fixture(case.name);
        for length in [0, 1, 27, 28, 57, original.len() / 2, original.len() - 1] {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&original[..length], &limits);
            }));
            assert!(
                result.is_ok(),
                "panicked on {} prefix of {length} bytes",
                case.name
            );
        }

        let footer = original.len() - 4;
        for offset in [
            40,
            41,
            42,
            43,
            44,
            45,
            58,
            58 + (footer - 58) / 3,
            58 + 2 * (footer - 58) / 3,
            footer - 1,
        ] {
            let mut mutated = original.clone();
            mutated[offset] ^= 0xa5;
            rewrite_inner_and_outer_crc(&mut mutated);
            let result = catch_unwind(AssertUnwindSafe(|| {
                let _ = decode_rgba(&mutated, &limits);
            }));
            assert!(
                result.is_ok(),
                "panicked on repaired {} mutation at {offset}",
                case.name
            );
        }
    }
}
