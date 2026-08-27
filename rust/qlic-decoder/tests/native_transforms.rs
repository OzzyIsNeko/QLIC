use std::fs;
use std::path::{Path, PathBuf};

use qlic_decoder::{Container, Limits, crc32, decode_rgba, parse_qst1};

fn repository_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
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

struct TransformCase {
    path: &'static str,
    width: u32,
    height: u32,
    mode: u8,
    transform: u8,
    tile_log: u8,
    flags: u8,
    control: u8,
    adaptation: u8,
    rgba_crc32: u32,
    samples: &'static [(usize, [u8; 4])],
}

fn assert_transform_case(case: &TransformCase) {
    let file = fs::read(repository_root().join(case.path)).unwrap();
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!(
        (stream.info.width, stream.info.height),
        (case.width, case.height)
    );
    assert_eq!(stream.info.channels, 4);
    assert_eq!(stream.info.mode, case.mode);
    assert_eq!(stream.info.transform, case.transform);
    assert_eq!(stream.info.tile_log, case.tile_log);
    assert_eq!(stream.info.flags, case.flags);
    assert_eq!(stream.info.control, case.control);
    assert_eq!(stream.info.adaptation, case.adaptation);
    assert_eq!(stream.info.pixel_crc32, case.rgba_crc32);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!((decoded.width, decoded.height), (case.width, case.height));
    assert_eq!(crc32(&decoded.rgba), case.rgba_crc32);
    for &(pixel, expected) in case.samples {
        let offset = pixel * 4;
        assert_eq!(&decoded.rgba[offset..offset + 4], expected);
    }
}

#[test]
fn decodes_retained_mode52_rgba_across_transform_families() {
    for case in [
        TransformCase {
            path: "retained/alpha-first-mode52-holdout-streams/009.qlic",
            width: 256,
            height: 256,
            mode: 52,
            transform: 0,
            tile_log: 4,
            flags: 0,
            control: 4,
            adaptation: 4,
            rgba_crc32: 0x6638_8335,
            samples: &[
                (0, [255, 255, 255, 0]),
                (2_160, [0, 0, 0, 1]),
                (3_185, [65, 51, 4, 97]),
                (3_195, [254, 201, 13, 255]),
            ],
        },
        TransformCase {
            path: "retained/alpha-first-mode52-holdout-streams/030.qlic",
            width: 64,
            height: 64,
            mode: 52,
            transform: 14,
            tile_log: 4,
            flags: 0,
            control: 4,
            adaptation: 4,
            rgba_crc32: 0x7de7_59e3,
            samples: &[
                (0, [0, 0, 0, 0]),
                (1_031, [0, 0, 0, 21]),
                (1_096, [63, 63, 61, 155]),
                (1_160, [70, 71, 68, 255]),
            ],
        },
        TransformCase {
            path: "retained/alpha-first-mode52-holdout-streams/024.qlic",
            width: 64,
            height: 64,
            mode: 52,
            transform: 29,
            tile_log: 4,
            flags: 0,
            control: 4,
            adaptation: 4,
            rgba_crc32: 0x7597_bdfe,
            samples: &[
                (0, [0, 0, 0, 0]),
                (398, [97, 97, 97, 29]),
                (463, [96, 96, 96, 255]),
            ],
        },
        TransformCase {
            path: "retained/alpha-first-mode52-discovery-streams/030.qlic",
            width: 64,
            height: 64,
            mode: 52,
            transform: 36,
            tile_log: 4,
            flags: 0,
            control: 0,
            adaptation: 5,
            rgba_crc32: 0x68c2_2bdf,
            samples: &[
                (0, [0, 0, 0, 0]),
                (456, [139, 139, 139, 11]),
                (521, [136, 138, 133, 255]),
            ],
        },
        TransformCase {
            path: "retained/alpha-first-mode52-discovery-streams/048.qlic",
            width: 128,
            height: 256,
            mode: 52,
            transform: 8,
            tile_log: 4,
            flags: 2,
            control: 0,
            adaptation: 5,
            rgba_crc32: 0xdb3d_94a8,
            samples: &[
                (0, [133, 132, 125, 0]),
                (1, [156, 152, 145, 0]),
                (127, [133, 132, 126, 0]),
                (128, [102, 98, 91, 0]),
                (16_384, [97, 95, 86, 0]),
                (32_767, [47, 44, 31, 0]),
            ],
        },
    ] {
        assert_transform_case(&case);
    }
}

#[test]
fn decodes_retained_mode45_and_mode53_rgba_lifts_and_weighted_transforms() {
    for case in [
        TransformCase {
            path: "retained/mode45-current-streams/0006.qlic",
            width: 1_024,
            height: 1_024,
            mode: 45,
            transform: 30,
            tile_log: 3,
            flags: 0,
            control: 0,
            adaptation: 5,
            rgba_crc32: 0xf176_a2f1,
            samples: &[
                (0, [0, 0, 0, 0]),
                (186_925, [192, 192, 191, 2]),
                (187_964, [170, 171, 171, 255]),
            ],
        },
        TransformCase {
            path: "retained/mode53-post-alpha-current-streams/0026.qlic",
            width: 640,
            height: 408,
            mode: 53,
            transform: 29,
            tile_log: 4,
            flags: 0,
            control: 0,
            adaptation: 5,
            rgba_crc32: 0x4fc3_07c5,
            samples: &[
                (0, [0, 0, 0, 0]),
                (36_867, [94, 98, 99, 2]),
                (38_150, [105, 104, 102, 255]),
            ],
        },
        TransformCase {
            path: "retained/mode53-post-alpha-current-streams/0020.qlic",
            width: 416,
            height: 800,
            mode: 53,
            transform: 37,
            tile_log: 4,
            flags: 0,
            control: 0,
            adaptation: 5,
            rgba_crc32: 0xa8ed_3b16,
            samples: &[
                (0, [255, 255, 255, 0]),
                (144, [246, 245, 245, 7]),
                (977, [188, 136, 231, 255]),
            ],
        },
    ] {
        assert_transform_case(&case);
    }
}

#[test]
fn decodes_production_transform39_stream_exactly() {
    let file =
        fs::read(repository_root().join("retained/sphere-normal-final-pairs/0004-candidate.qlic"))
            .unwrap();
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (128, 256));
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.mode, 52);
    assert_eq!(stream.info.transform, 39);
    assert_eq!(stream.info.tile_log, 4);
    assert_eq!(stream.info.pixel_crc32, 0xeeb6_ebc9);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!(crc32(&decoded.rgba), 0x9dd0_c466);
    for (pixel, expected) in [
        (0, [72, 77, 231, 255]),
        (1, [77, 54, 219, 255]),
        (127, [173, 51, 218, 255]),
        (128, [52, 83, 220, 255]),
        (16_384, [39, 116, 218, 255]),
        (32_767, [187, 196, 217, 255]),
    ] {
        assert_eq!(&decoded.rgba[pixel * 4..pixel * 4 + 4], expected);
    }
}

#[test]
fn decodes_production_transform40_stream_exactly() {
    let file = fs::read(repository_root().join("normal-map-sphere-green8.qlic")).unwrap();
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let stream = parse_qst1(outer.payload, &Limits::default()).unwrap();
    assert_eq!((stream.info.width, stream.info.height), (128, 256));
    assert_eq!(stream.info.channels, 3);
    assert_eq!(stream.info.mode, 52);
    assert_eq!(stream.info.transform, 40);
    assert_eq!(stream.info.tile_log, 4);
    assert_eq!(stream.info.adaptation, 4);
    assert_eq!(stream.info.pixel_crc32, 0x5c36_171a);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!(crc32(&decoded.rgba), 0x9122_c65a);
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
fn constant_alpha_control_byte_reconstructs_nonzero_alpha_without_an_entropy_plane() {
    let mut file =
        fs::read(repository_root().join("retained/alpha-first-mode52-discovery-streams/048.qlic"))
            .unwrap();
    let decoded_zero = decode_rgba(&file, &Limits::default()).unwrap();
    assert!(decoded_zero.rgba.chunks_exact(4).all(|pixel| pixel[3] == 0));

    let mut expected = decoded_zero.rgba;
    for pixel in expected.chunks_exact_mut(4) {
        pixel[3] = 173;
    }
    file[28 + 17] = 173;
    file[28 + 18..28 + 22].copy_from_slice(&crc32(&expected).to_le_bytes());
    rewrite_inner_and_outer_crc(&mut file);

    let decoded = decode_rgba(&file, &Limits::default()).unwrap();
    assert_eq!(decoded.rgba, expected);
}
