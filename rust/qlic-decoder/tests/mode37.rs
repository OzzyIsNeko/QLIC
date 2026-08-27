use std::collections::BTreeSet;
use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Error, LimitKind, Limits, crc32, decode_rgba, parse_qst1};

const OUTER_HEADER_SIZE: usize = 28;
const OUTER_FOOTER_SIZE: usize = 4;
const QST1_CHANNELS_OFFSET: usize = OUTER_HEADER_SIZE + 12;
const QST1_FLAGS_OFFSET: usize = OUTER_HEADER_SIZE + 13;
const QST1_CONTROL_OFFSET: usize = OUTER_HEADER_SIZE + 17;
const QST1_PIXEL_CRC_OFFSET: usize = OUTER_HEADER_SIZE + 18;
const QST1_CONTAINER_CRC_OFFSET: usize = OUTER_HEADER_SIZE + 26;

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn retained_directory() -> PathBuf {
    repository_root().join("retained/mode37-post-alpha-current-streams")
}

fn retained_stream(index: u16) -> Vec<u8> {
    fs::read(retained_directory().join(format!("{index:04}.qlic"))).unwrap()
}

fn rewrite_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - OUTER_FOOTER_SIZE;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn rewrite_inner_and_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - OUTER_FOOTER_SIZE;
    let mut qst1 = bytes[OUTER_HEADER_SIZE..footer].to_vec();
    qst1[26..30].fill(0);
    let checksum = crc32(&qst1).to_le_bytes();
    bytes[QST1_CONTAINER_CRC_OFFSET..QST1_CONTAINER_CRC_OFFSET + 4].copy_from_slice(&checksum);
    rewrite_outer_crc(bytes);
}

struct Mode37Case {
    index: u16,
    width: u32,
    height: u32,
    channels: u8,
    transform: u8,
    tile_log: u8,
    control: u8,
    pixel_crc32: u32,
}

#[test]
fn decodes_high_signal_mode37_streams_across_channel_and_transform_families() {
    let cases = [
        Mode37Case {
            index: 115,
            width: 343,
            height: 480,
            channels: 1,
            transform: 0,
            tile_log: 3,
            control: 6,
            pixel_crc32: 0xdb4c_b410,
        },
        Mode37Case {
            index: 143,
            width: 256,
            height: 32,
            channels: 3,
            transform: 0,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0x769d_622d,
        },
        Mode37Case {
            index: 124,
            width: 16,
            height: 32,
            channels: 3,
            transform: 2,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0x7200_30a1,
        },
        Mode37Case {
            index: 111,
            width: 952,
            height: 1_058,
            channels: 3,
            transform: 5,
            tile_log: 4,
            control: 0,
            pixel_crc32: 0x56ed_5859,
        },
        Mode37Case {
            index: 59,
            width: 64,
            height: 64,
            channels: 4,
            transform: 10,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0xc2e2_facc,
        },
        Mode37Case {
            index: 70,
            width: 64,
            height: 64,
            channels: 4,
            transform: 29,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0xcdf5_1a05,
        },
        Mode37Case {
            index: 42,
            width: 64,
            height: 64,
            channels: 4,
            transform: 31,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0xa546_b4d1,
        },
        Mode37Case {
            index: 13,
            width: 64,
            height: 64,
            channels: 4,
            transform: 35,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0x51f1_4075,
        },
        Mode37Case {
            index: 71,
            width: 64,
            height: 64,
            channels: 4,
            transform: 36,
            tile_log: 4,
            control: 4,
            pixel_crc32: 0x805b_de29,
        },
    ];

    for case in cases {
        let file = retained_stream(case.index);
        let outer = Container::parse(&file, &Limits::default()).unwrap();
        let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
        assert_eq!(
            (stream.info.width, stream.info.height),
            (case.width, case.height)
        );
        assert_eq!(stream.info.channels, case.channels);
        assert_eq!(stream.info.flags, 0);
        assert_eq!(stream.info.mode, 37);
        assert_eq!(stream.info.transform, case.transform);
        assert_eq!(stream.info.tile_log, case.tile_log);
        assert_eq!(stream.info.control, case.control);
        assert_eq!(stream.info.pixel_crc32, case.pixel_crc32);

        let decoded = decode_rgba(&file, &Limits::default()).unwrap();
        assert_eq!((decoded.width, decoded.height), (case.width, case.height));
        assert_eq!(
            decoded.rgba.len(),
            usize::try_from(case.width).unwrap() * usize::try_from(case.height).unwrap() * 4
        );
        if case.channels != 4 {
            assert!(decoded.rgba.chunks_exact(4).all(|pixel| pixel[3] == 255));
        }
        if case.channels == 1 {
            assert!(
                decoded
                    .rgba
                    .chunks_exact(4)
                    .all(|pixel| pixel[0] == pixel[1] && pixel[1] == pixel[2])
            );
        }
    }
}

#[test]
fn decodes_all_curated_mode37_streams_exactly() {
    let mut paths = fs::read_dir(retained_directory())
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "qlic")
        })
        .collect::<Vec<_>>();
    paths.sort_unstable();

    let mut channels = [0_usize; 5];
    let mut transforms = BTreeSet::new();
    let mut total_bytes = 0_u64;
    assert_eq!(paths.len(), 9);

    for path in paths {
        let file = fs::read(&path).unwrap();
        total_bytes = total_bytes.checked_add(file.len() as u64).unwrap();
        let outer = Container::parse(&file, &Limits::default()).unwrap();
        let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
        assert_eq!(stream.info.mode, 37, "{}", path.display());
        assert_eq!(stream.info.flags, 0, "{}", path.display());
        channels[usize::from(stream.info.channels)] += 1;
        transforms.insert(stream.info.transform);

        let decoded = decode_rgba(&file, &Limits::default())
            .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
        assert_eq!(
            (decoded.width, decoded.height),
            (stream.info.width, stream.info.height),
            "{}",
            path.display()
        );
        match stream.info.channels {
            1 => {
                assert!(decoded.rgba.chunks_exact(4).all(|pixel| {
                    pixel[0] == pixel[1] && pixel[1] == pixel[2] && pixel[3] == 255
                }))
            }
            3 => assert!(decoded.rgba.chunks_exact(4).all(|pixel| pixel[3] == 255)),
            4 => {}
            channels => panic!("unexpected channel count {channels} in {}", path.display()),
        }
    }

    assert_eq!(total_bytes, 933_327);
    assert_eq!(channels, [0, 1, 0, 3, 5]);
    assert_eq!(transforms, BTreeSet::from([0, 2, 5, 10, 29, 31, 35, 36]));
}

#[test]
fn mode37_constant_alpha_uses_the_control_byte_without_an_alpha_plane() {
    // Constant-alpha streams store alpha in the control byte, so their entropy
    // adaptation is the default. Start from an RGB stream encoded with that
    // same adaptation to isolate the missing fourth entropy plane.
    let mut file = retained_stream(111);
    let original = decode_rgba(&file, &Limits::default()).unwrap();
    let mut expected = original.rgba;
    for pixel in expected.chunks_exact_mut(4) {
        pixel[3] = 173;
    }

    file[QST1_CHANNELS_OFFSET] = 4;
    file[QST1_FLAGS_OFFSET] = 2;
    file[QST1_CONTROL_OFFSET] = 173;
    file[QST1_PIXEL_CRC_OFFSET..QST1_PIXEL_CRC_OFFSET + 4]
        .copy_from_slice(&crc32(&expected).to_le_bytes());
    rewrite_inner_and_outer_crc(&mut file);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!(decoded.rgba, expected);
}

#[test]
fn mode37_enforces_limits_and_rejects_damage_without_panicking() {
    let file = retained_stream(115);
    let decoded_bytes = 343 * 480 * 4;
    let limits = Limits {
        max_decoded_bytes: decoded_bytes - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decode_rgba(&file, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual,
            ..
        }) if actual == decoded_bytes
    ));

    for length in [0, 1, 27, 28, 57, file.len() / 2, file.len() - 1] {
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&file[..length], &Limits::default());
        }));
        assert!(result.is_ok(), "panicked on a {length}-byte prefix");
    }

    for offset in [
        OUTER_HEADER_SIZE,
        QST1_CHANNELS_OFFSET,
        QST1_FLAGS_OFFSET,
        QST1_CONTROL_OFFSET,
        QST1_PIXEL_CRC_OFFSET,
        file.len() / 2,
        file.len() - OUTER_FOOTER_SIZE - 1,
    ] {
        let mut mutated = file.clone();
        mutated[offset] ^= 0xa5;
        rewrite_inner_and_outer_crc(&mut mutated);
        let result = catch_unwind(AssertUnwindSafe(|| {
            let _ = decode_rgba(&mutated, &Limits::default());
        }));
        assert!(
            result.is_ok(),
            "panicked on a rechecksummed mutation at {offset}"
        );
    }
}
