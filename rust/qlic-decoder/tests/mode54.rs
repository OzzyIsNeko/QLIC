use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Error, Limits, crc32, decode_rgba, parse_qst1};

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn retained_profile(name: &str) -> Vec<u8> {
    fs::read(
        repository_root()
            .join("retained/mode37-kodak-waterloo-holdout-streams")
            .join(name),
    )
    .unwrap()
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

struct Mode54Case {
    name: &'static str,
    width: u32,
    height: u32,
    transform: u8,
    tile_log: u8,
    rgb_crc32: u32,
    rgba_crc32: u32,
    samples: &'static [(usize, [u8; 4])],
}

const CASES: &[Mode54Case] = &[
    Mode54Case {
        name: "0005-kodim05.qlic",
        width: 768,
        height: 512,
        transform: 32,
        tile_log: 3,
        rgb_crc32: 0xfa4b_95d0,
        rgba_crc32: 0x341e_54c9,
        samples: &[
            (0, [99, 99, 99, 255]),
            (76_900, [135, 130, 114, 255]),
            (393_215, [0, 0, 0, 255]),
        ],
    },
    Mode54Case {
        name: "0007-kodim07.qlic",
        width: 768,
        height: 512,
        transform: 33,
        tile_log: 4,
        rgb_crc32: 0x3f3d_44b3,
        rgba_crc32: 0x58a6_b0c5,
        samples: &[
            (0, [99, 99, 99, 255]),
            (76_900, [132, 131, 111, 255]),
            (393_215, [0, 0, 0, 255]),
        ],
    },
    Mode54Case {
        name: "0014-kodim14.qlic",
        width: 768,
        height: 512,
        transform: 32,
        tile_log: 4,
        rgb_crc32: 0x71b0_43f9,
        rgba_crc32: 0xf08a_349e,
        samples: &[
            (0, [99, 99, 99, 255]),
            (76_900, [153, 171, 145, 255]),
            (393_215, [0, 0, 0, 255]),
        ],
    },
    Mode54Case {
        name: "0018-kodim18.qlic",
        width: 512,
        height: 768,
        transform: 32,
        tile_log: 4,
        rgb_crc32: 0xbcd5_4731,
        rgba_crc32: 0xa77e_396c,
        samples: &[
            (0, [17, 12, 13, 255]),
            (51_300, [88, 80, 68, 255]),
            (393_215, [172, 157, 109, 255]),
        ],
    },
    Mode54Case {
        name: "0022-kodim22.qlic",
        width: 768,
        height: 512,
        transform: 35,
        tile_log: 4,
        rgb_crc32: 0x0770_4147,
        rgba_crc32: 0x926d_4779,
        samples: &[
            (0, [184, 163, 150, 255]),
            (76_900, [120, 110, 91, 255]),
            (393_215, [0, 0, 0, 255]),
        ],
    },
];

#[test]
fn decodes_all_five_retained_mode54_weighted_streams_exactly() {
    for case in CASES {
        let file = retained_profile(case.name);
        let outer = Container::parse(&file, &Limits::default()).unwrap();
        let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
        assert_eq!(
            (stream.info.width, stream.info.height),
            (case.width, case.height)
        );
        assert_eq!(stream.info.channels, 3);
        assert_eq!(stream.info.flags, 0);
        assert_eq!(stream.info.mode, 54);
        assert_eq!(stream.info.transform, case.transform);
        assert_eq!(stream.info.tile_log, case.tile_log);
        assert_eq!(stream.info.adaptation, 5);
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
fn mode54_enforces_limits_and_keeps_neighboring_forms_explicit() {
    let file = retained_profile(CASES[0].name);
    for limits in [
        Limits {
            max_pixels: 768 * 512 - 1,
            ..Limits::default()
        },
        Limits {
            max_decoded_bytes: 768 * 512 * 4 - 1,
            ..Limits::default()
        },
    ] {
        assert!(matches!(
            decode_rgba(&file, &limits),
            Err(Error::LimitExceeded { .. })
        ));
    }

    let mut channels = file.clone();
    channels[28 + 12] = 4;
    rewrite_inner_and_outer_crc(&mut channels);
    assert!(matches!(
        decode_rgba(&channels, &Limits::default()),
        Err(Error::UnsupportedQst1Channels {
            mode: 54,
            channels: 4
        })
    ));

    let mut flags = file.clone();
    flags[28 + 13] = 1;
    flags[28 + 15] = 0;
    rewrite_inner_and_outer_crc(&mut flags);
    assert!(matches!(
        decode_rgba(&flags, &Limits::default()),
        Err(Error::UnsupportedQst1Flags { mode: 54, flags: 1 })
    ));

    for transform in [0, 31, 34, 36, 38] {
        let mut unsupported = file.clone();
        unsupported[28 + 15] = transform;
        rewrite_inner_and_outer_crc(&mut unsupported);
        assert!(matches!(
            decode_rgba(&unsupported, &Limits::default()),
            Err(Error::UnsupportedQst1Transform {
                mode: 54,
                transform: actual
            }) if actual == transform
        ));
    }

    let mut neighboring_mode = file.clone();
    neighboring_mode[28 + 14] = 51;
    rewrite_inner_and_outer_crc(&mut neighboring_mode);
    assert!(matches!(
        decode_rgba(&neighboring_mode, &Limits::default()),
        Err(Error::UnsupportedQst1Mode(51))
    ));
}

#[test]
fn mode54_rejects_dimension_overflow_without_allocating_pixels() {
    let mut file = retained_profile(CASES[0].name);
    for offset in [4, 8, 28 + 4, 28 + 8] {
        file[offset..offset + 4].copy_from_slice(&u32::MAX.to_le_bytes());
    }
    rewrite_inner_and_outer_crc(&mut file);
    let limits = Limits {
        max_pixels: u64::MAX,
        max_decoded_bytes: u64::MAX,
        ..Limits::default()
    };
    let result = catch_unwind(AssertUnwindSafe(|| decode_rgba(&file, &limits)));
    assert!(result.is_ok());
    assert!(matches!(result.unwrap(), Err(Error::ArithmeticOverflow(_))));
}

#[test]
fn mode54_public_decode_never_panics_on_truncation_or_rechecksummed_mutation() {
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 20,
        max_decoded_bytes: 4 << 20,
        ..Limits::default()
    };
    for case in CASES {
        let original = retained_profile(case.name);
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
        let payload_begin = 58;
        for offset in [
            40,
            41,
            42,
            43,
            44,
            45,
            payload_begin,
            payload_begin + (footer - payload_begin) / 4,
            payload_begin + (footer - payload_begin) / 2,
            payload_begin + 3 * (footer - payload_begin) / 4,
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
                "panicked on rechecksummed {} mutation at {offset}",
                case.name
            );
        }
    }
}
